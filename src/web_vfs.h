#pragma once

#include "HTTP.h"
#include "SQLiteVFS.h"
#include "ThreadPool.h"
#include <set>

namespace WebVFS {

using std::cerr;
using std::endl;

class Timer {
    unsigned long long t0_;

  public:
    Timer() {
        timeval t0;
        gettimeofday(&t0, nullptr);
        t0_ = t0.tv_sec * 1000000ULL + t0.tv_usec;
    }

    unsigned long long micros() {
        timeval tv;
        gettimeofday(&tv, nullptr);
        return tv.tv_sec * 1000000ULL + tv.tv_usec - t0_;
    }
};

// Extent: a contiguous portion of the remote file which we keep cached in memory. Three size
// options to balance latency, throughput, read amplification, HTTP request count.
struct Extent {
    enum Size { SM, MD, LG };

    Size size;
    size_t rank;

    static size_t Bytes(Size sz) {
        switch (sz) {
        case Size::SM:
            return 65536;
        case Size::MD:
            return 1048576;
        }
        return 16777216;
    }

    static Extent Containing(uint64_t offset, size_t length) {
        auto rk = offset / Bytes(Size::SM);
        auto hi = offset + std::max(length, size_t(1)) - 1;
        if (rk == hi / Bytes(Size::SM)) {
            return Extent(Size::SM, rk);
        }
        throw std::runtime_error("unaligned Read");
    }

    Extent() : size(Size::SM), rank(0) {}
    Extent(Size size_, size_t rank_) : size(size_), rank(rank_) {}
    bool operator<(const Extent &rhs) const {
        return size < rhs.size || (size == rhs.size && rank < rhs.rank);
    }

    Extent Promote() const {
        if (size == Size::SM) {
            return Extent(Size::MD, rank / 16);
        }
        if (size == Size::MD) {
            return Extent(Size::LG, rank / 16);
        }
        return *this;
    }

    uint64_t Offset() const { return Bytes(size) * rank; }

    size_t Bytes() const { return Bytes(size); }

    bool Contains(uint64_t offset, size_t length) const {
        return offset >= Offset() && offset + length <= Offset() + Bytes();
    }

    bool Contains(const Extent &rhs) const { return Contains(rhs.Offset(), rhs.Bytes()); }

    bool Exists(uint64_t file_size_) const { return Offset() < file_size_; }

    std::string str(uint64_t file_size) const {
        auto hi = std::min((Offset() + Bytes() - 1), file_size - 1);
        std::ostringstream fmt_range;
        fmt_range << "bytes=" << Offset() << "-" << hi;
        return fmt_range.str();
    }
};

class File : public SQLiteVFS::File {
    const std::string uri_, filename_;
    const sqlite_int64 file_size_;
    const std::unique_ptr<HTTP::CURLpool> curlpool_;
    const unsigned long log_level_ = 1;

    // Extents cached for potential reuse, with a last-use timestamp for LRU eviction.
    // Note: The purpose of keeping extents cached is to anticipate future Read requests for nearby
    //       database pages. It is NOT to serve repeat Reads for the same page, which is the job of
    //       the SQLite page cache in front of the VFS.
    using shared_string = std::shared_ptr<const std::string>;
    struct ResidentExtent {
        const Extent extent;
        shared_string data;
        uint64_t seqno;
        bool used = false;
        ResidentExtent(Extent extent_, const shared_string &data_, uint64_t seqno_)
            : extent(extent_), data(data_), seqno(seqno_) {}
    };
    std::map<Extent, ResidentExtent> resident_;
    std::map<uint64_t, Extent> usage_; // secondary index of resident_ ordered by seqno (for LRU)
    uint64_t extent_seqno_ = 0;        // timestamp
    ThreadPoolWithEnqueueFast threadpool_;

    // The main thread locks mu_ to update resident_ and usage_, but may read them without.
    // Background threads may (only) read them with mu_ locked.
    // The other state below always requires mu_ to read or write.
    std::mutex mu_;

    // state for [pre]fetch on background threads
    std::deque<Extent> fetch_queue_;             // queue of fetch ops
    std::set<Extent> fetch_queue2_;              // secondary index of fetch_queue_
    std::set<Extent> fetch_wip_;                 // fetch ops currently underway
    std::map<Extent, shared_string> fetch_done_; // completed fetches waiting to be merged
    std::map<Extent, int> fetch_error_;          // error codes
    std::condition_variable fetch_cv_;           // on add to fetch_done_ or fetch_error_

    // run HTTP GET request for an extent
    int FetchExtent(Extent extent, shared_string &data) {
        Timer t;
        const std::string protocol = uri_.substr(0, 6) == "https:" ? "HTTPS" : "HTTP";
        try {
            HTTP::headers reqhdrs, reshdrs;
            reqhdrs["range"] = extent.str(file_size_);

            long status = -1;
            bool retried = false;
            std::shared_ptr<std::string> body(new std::string());
            HTTP::RetryOptions options;
            options.min_response_body =
                std::min(uint64_t(extent.Bytes()), uint64_t(file_size_ - extent.Offset()));
            options.connpool = curlpool_.get();
            options.on_retry = [&retried](HTTP::Method method, const std::string &url,
                                          const HTTP::headers &request_headers, CURLcode rc,
                                          long response_code, const HTTP::headers &response_headers,
                                          const std::string &response_body,
                                          unsigned int attempt) { retried = true; };
            auto rc = HTTP::RetryGet(uri_, reqhdrs, status, reshdrs, *body, options);
            if (rc != CURLE_OK) {
                if (log_level_) {
                    std::lock_guard<std::mutex> lock(mu_);
                    cerr << protocol << " GET " << filename_ << ' ' << reqhdrs["range"] << ' '
                         << curl_easy_strerror(rc) << endl
                         << std::flush;
                }
                return SQLITE_IOERR_READ;
            }
            if (status < 200 || status >= 300) {
                if (log_level_) {
                    std::lock_guard<std::mutex> lock(mu_);
                    cerr << protocol << " GET " << filename_ << ' ' << reqhdrs["range"]
                         << " error status = " << status << endl
                         << std::flush;
                }
                return SQLITE_IOERR_READ;
            }
            if (body->size() != options.min_response_body) {
                if (log_level_) {
                    std::lock_guard<std::mutex> lock(mu_);
                    cerr << protocol << " GET " << filename_ << ' ' << reqhdrs["range"]
                         << " incorrect response body length = " << body->size()
                         << ", expected = " << extent.Bytes() << endl
                         << std::flush;
                }
                return SQLITE_IOERR_SHORT_READ;
            }
            data = body;
            if (log_level_ > 1) {
                std::lock_guard<std::mutex> lock(mu_);
                cerr << protocol << " GET " << filename_ << ' ' << reqhdrs["range"] << " OK ("
                     << (t.micros() / 1000) << "ms)" << endl
                     << std::flush;
            } else if (log_level_ && retried) {
                std::lock_guard<std::mutex> lock(mu_);
                cerr << protocol << " GET " << filename_ << ' ' << reqhdrs["range"]
                     << " OK after retry (" << (t.micros() / 1000) << "ms)" << endl
                     << std::flush;
            }
            return SQLITE_OK;
        } catch (std::bad_alloc &) {
            return SQLITE_IOERR_NOMEM;
        } catch (std::exception &exn) {
            if (log_level_) {
                std::lock_guard<std::mutex> lock(mu_);
                cerr << protocol << " GET " << filename_ << ": " << exn.what() << endl
                     << std::flush;
            }
            return SQLITE_IOERR_READ;
        }
    }

    static void *FetchJob(void *v) {
        File *self = (File *)v;

        std::unique_lock<std::mutex> lock(self->mu_);

        // dequeue a desired extent
        Extent extent = self->fetch_queue_.front();
        self->fetch_queue_.pop_front();
        self->fetch_queue2_.erase(extent);

        // coalesce request if same extent or one containing it is already wip or done
        Extent container = extent;
        for (int i = 0; i < 3; ++i) {
            if (self->resident_.find(container) != self->resident_.end() ||
                self->fetch_wip_.find(container) != self->fetch_wip_.end() ||
                self->fetch_done_.find(container) != self->fetch_done_.end()) {
                return nullptr;
            }
            container = container.Promote();
        }

        // run HTTP GET
        self->fetch_wip_.insert(extent);
        lock.unlock();
        shared_string buf;
        int rc = self->FetchExtent(extent, buf);
        lock.lock();

        // record result
        self->fetch_wip_.erase(extent);
        assert(self->fetch_done_.find(extent) == self->fetch_done_.end() &&
               self->fetch_error_.find(extent) == self->fetch_error_.end());
        if (rc == SQLITE_OK) {
            self->fetch_done_.emplace(extent, buf);
        } else {
            self->fetch_error_[extent] = rc;
        }
        lock.unlock();
        self->fetch_cv_.notify_all();

        return nullptr;
    }

    void UpdateResident(std::unique_lock<std::mutex> &lock) {
        // main thread collects results of recent background fetch jobs
        assert(lock.owns_lock());

        // surface any errors recorded
        auto err = fetch_error_.begin();
        if (err != fetch_error_.end()) {
            int rc = err->second;
            fetch_error_.erase(err);
            throw rc;
        }

        // merge successfully fetched extents into resident_ (& update usage_)
        for (auto p = fetch_done_.begin(); p != fetch_done_.end(); p = fetch_done_.erase(p)) {
            Extent extent = p->first;
            if (resident_.find(extent) == resident_.end()) {
                usage_[++extent_seqno_] = extent;
                resident_.emplace(extent, ResidentExtent(extent, p->second, extent_seqno_));
            }
        }
    }

    std::map<Extent, ResidentExtent>::iterator FindResidentExtent(uint64_t offset, size_t length) {
        Extent extent = Extent::Containing(offset, length);
        auto line = resident_.find(extent);
        for (int i = 0; i < 2 && line == resident_.end(); ++i) {
            line = resident_.find((extent = extent.Promote()));
        }
        return line;
    }

    bool ResidentAndUsed(Extent extent) {
        auto p = resident_.find(Extent(extent.size, extent.rank - 1));
        return p != resident_.end() && p->second.used;
    }

    ResidentExtent EnsureResidentExtent(uint64_t offset, size_t length) {
        auto last_used = usage_.rbegin();
        if (last_used != usage_.rend() && last_used->second.Contains(offset, length)) {
            // most-recently used extent still has the page we need (hot path, doesn't lock mu_)
            auto p = resident_.find(last_used->second);
            assert(p != resident_.end());
            return p->second;
        }
        std::unique_lock<std::mutex> lock(mu_);
        // find a resident extent containing desired page
        auto line = FindResidentExtent(offset, length);
        if (line == resident_.end()) {
            // page isn't resident; first, check if it's among any recently-completed fetches
            UpdateResident(lock);
            if ((line = FindResidentExtent(offset, length)) == resident_.end()) {
                // front-enqueue job to fetch the extent
                Extent extent = Extent::Containing(offset, length);
                // if we already have an adjacent extent (at any size), promote to next size up
                if (extent.rank > 0 && Extent(extent.size, extent.rank + 1).Exists(file_size_)) {
                    Extent container = extent;
                    for (int i = 0; i < 2; ++i) {
                        if (container.rank > 0 &&
                                resident_.find(Extent(container.size, container.rank - 1)) !=
                                    resident_.end() ||
                            resident_.find(Extent(container.size, container.rank + 1)) !=
                                resident_.end()) {
                            extent = container.Promote();
                        }
                        container = container.Promote();
                    }
                }
                if (fetch_queue2_.find(extent) == fetch_queue2_.end()) {
                    fetch_queue_.push_front(extent);
                    fetch_queue2_.insert(extent);
                    threadpool_.EnqueueFast(this, FetchJob, nullptr);
                }
                do {
                    fetch_cv_.wait(lock);
                    UpdateResident(lock);
                } while ((line = FindResidentExtent(offset, length)) == resident_.end());
            }
        } else {
            Extent extent = line->first;
            // upon finding the desired page in a resident extent, decide whether to initiate
            // background prefetch operations for other, nearby extents
            if (extent.size < Extent::Size::LG) {
                // if we used r-1 or r+1, initiate prefetch of the next size up
                Extent promoted = extent.Promote();
                if ((extent.rank > 0 && ResidentAndUsed(Extent(extent.size, extent.rank - 1)) ||
                     ResidentAndUsed(Extent(extent.size, extent.rank + 1))) &&
                    fetch_queue2_.find(promoted) == fetch_queue2_.end() &&
                    resident_.find(promoted) == resident_.end()) {
                    fetch_queue_.push_back(promoted);
                    fetch_queue2_.insert(promoted);
                    threadpool_.EnqueueFast(this, FetchJob, nullptr);
                }
            } else {
                // large size: if we used r-1 resident, prefetch r+1, and vice versa
                for (int ofs = 1; ofs <= 4; ++ofs) {
                    if (extent.rank >= ofs &&
                        ResidentAndUsed(Extent(Extent::Size::LG, extent.rank - ofs))) {
                        Extent succ = Extent(Extent::Size::LG, extent.rank + ofs);
                        if (succ.Exists(file_size_) && resident_.find(succ) == resident_.end() &&
                            fetch_queue2_.find(succ) == fetch_queue2_.end()) {
                            fetch_queue_.push_back(succ);
                            fetch_queue2_.insert(succ);
                            threadpool_.EnqueueFast(this, FetchJob, nullptr);
                        }
                    } else {
                        break;
                    }
                }
                for (int ofs = 1; ofs <= 4; ++ofs) {
                    if (extent.rank >= ofs &&
                        ResidentAndUsed(Extent(Extent::Size::LG, extent.rank + ofs))) {
                        Extent pred = Extent(Extent::Size::LG, extent.rank - ofs);
                        if (resident_.find(pred) == resident_.end() &&
                            fetch_queue2_.find(pred) == fetch_queue2_.end()) {
                            fetch_queue_.push_back(pred);
                            fetch_queue2_.insert(pred);
                            threadpool_.EnqueueFast(this, FetchJob, nullptr);
                        }
                    } else {
                        break;
                    }
                }
            }
        }

        // LRU bookkeeping: delete old seqno_ from the secondary index, then record the new one
        assert(!(line->first < line->second.extent || line->second.extent < line->first));
        assert(usage_.find(line->second.seqno) != usage_.end());
        usage_.erase(line->second.seqno); // old seqno
        usage_[++extent_seqno_] = line->first;
        line->second.seqno = extent_seqno_;
        line->second.used = true;

        // evict LRU extents if needed; this comes last to ensure we won't evict the extent we
        // just decided to use!
        while (resident_.size() > 40) {
            auto lru = usage_.begin();
            assert(lru->second < line->first || line->first < lru->second);
            resident_.erase(resident_.find(lru->second));
            usage_.erase(lru->first);
        }

        return line->second;
    }

    int Read(void *zBuf, int iAmt, sqlite3_int64 iOfst) override {
        if (iAmt == 0) {
            return SQLITE_OK;
        }
        if (iAmt < 0 || iOfst < 0) {
            return SQLITE_IOERR_READ;
        }
        try {
            ResidentExtent resext = EnsureResidentExtent(iOfst, iAmt);
            assert(resext.extent.Contains(iOfst, iAmt));
            assert(resext.data->size() + resext.extent.Offset() >= iOfst + iAmt);
            memcpy(zBuf, resext.data->c_str() + (iOfst - resext.extent.Offset()), iAmt);
            return SQLITE_OK;
        } catch (int rc) {
            return rc != SQLITE_OK ? rc : SQLITE_IOERR_READ;
        } catch (std::bad_alloc &) {
            return SQLITE_IOERR_NOMEM;
        }
    }
    int Write(const void *zBuf, int iAmt, sqlite3_int64 iOfst) override { return SQLITE_MISUSE; }
    int Truncate(sqlite3_int64 size) override { return SQLITE_MISUSE; }
    int Sync(int flags) override { return SQLITE_MISUSE; }
    int FileSize(sqlite3_int64 *pSize) override {
        *pSize = (sqlite3_int64)file_size_;
        return SQLITE_OK;
    }
    int Lock(int eLock) override { return SQLITE_OK; }
    int Unlock(int eLock) override { return SQLITE_OK; }
    int CheckReservedLock(int *pResOut) override {
        *pResOut = 0;
        return SQLITE_OK;
    }
    int FileControl(int op, void *pArg) override { return SQLITE_NOTFOUND; }
    int SectorSize() override { return 0; }
    int DeviceCharacteristics() override { return SQLITE_IOCAP_IMMUTABLE; }

    int ShmMap(int iPg, int pgsz, int isWrite, void volatile **pp) override {
        return SQLITE_MISUSE;
    }
    int ShmLock(int offset, int n, int flags) override { return SQLITE_MISUSE; }
    void ShmBarrier() override {}
    int ShmUnmap(int deleteFlag) override { return SQLITE_MISUSE; }

    int Fetch(sqlite3_int64 iOfst, int iAmt, void **pp) override { return SQLITE_MISUSE; }
    int Unfetch(sqlite3_int64 iOfst, void *p) override { return SQLITE_MISUSE; }

  public:
    File(const std::string &uri, const std::string &filename, sqlite_int64 file_size,
         std::unique_ptr<HTTP::CURLpool> &&curlpool, unsigned long log_level = 1)
        : uri_(uri), filename_(filename), file_size_(file_size), curlpool_(std::move(curlpool)),
          log_level_(log_level), threadpool_(4, 4) {
        methods_.iVersion = 1;
    }
    virtual ~File() { threadpool_.Barrier(); }
};

class VFS : public SQLiteVFS::Wrapper {
  protected:
    std::string last_error_;

    int Open(const char *zName, sqlite3_file *pFile, int flags, int *pOutFlags) override {
        if (!zName || strcmp(zName, "/__web__")) {
            return wrapped_->xOpen(wrapped_, zName, pFile, flags, pOutFlags);
        }
        const char *encoded_uri = sqlite3_uri_parameter(zName, "web_uri");
        if (!encoded_uri || !encoded_uri[0]) {
            last_error_ = "set web_uri query parameter to URI-encoded URI";
            return SQLITE_CANTOPEN;
        }
        if (!(flags & SQLITE_OPEN_READONLY)) {
            last_error_ = "web access is read-only";
            return SQLITE_CANTOPEN;
        }
        unsigned long log_level = 1;
        const char *env_log = getenv("SQLITE_WEB_LOG");
        if (env_log && *env_log) {
            log_level = strtoul(env_log, nullptr, 10);
            if (log_level == ULONG_MAX) {
                log_level = 1;
            }
        }

        // get desired URI
        try {
            std::string uri;
            std::unique_ptr<HTTP::CURLpool> curlpool;
            curlpool.reset(new HTTP::CURLpool(4));
            auto conn = curlpool->checkout();
            if (!conn->unescape(encoded_uri, uri)) {
                last_error_ = "Failed URI-decoding web_uri";
                return SQLITE_CANTOPEN;
            }
            curlpool->checkin(conn);
            std::string filename = FileNameForLog(uri);
            const std::string protocol = uri.substr(0, 6) == "https:" ? "HTTPS" : "HTTP";
            Timer t;

            long long file_size = sqlite3_uri_int64(zName, "web_content_length", -1);
            if (file_size <= 0) {
                // HEAD request to determine the database file's existence & size
                HTTP::headers reqhdrs, reshdrs;
                long status = -1;
                HTTP::RetryOptions options;
                options.connpool = curlpool.get();
                CURLcode rc = HTTP::RetryHead(uri, reqhdrs, status, reshdrs, options);
                if (rc != CURLE_OK) {
                    last_error_ = protocol + " HEAD " + filename + ": ";
                    last_error_ += curl_easy_strerror(rc);
                    if (log_level) {
                        cerr << last_error_ << endl;
                    }
                    return SQLITE_IOERR_READ;
                }
                if (status < 200 || status >= 300) {
                    last_error_ = protocol + " HEAD " + filename +
                                  ": error status = " + std::to_string(status);
                    if (log_level) {
                        cerr << last_error_ << endl;
                    }
                    return SQLITE_CANTOPEN;
                }

                // parse content-length
                file_size = HTTP::ReadContentLengthHeader(reshdrs);
                if (file_size < 0) {
                    last_error_ = protocol + " HEAD " + filename +
                                  ":response lacking valid content-length header";
                    if (log_level) {
                        cerr << last_error_ << endl;
                    }
                    return SQLITE_IOERR_READ;
                }
                if (log_level > 1) {
                    cerr << protocol << " HEAD " << filename << " content-length: " << file_size
                         << " (" << (t.micros() / 1000) << "ms)" << endl;
                }
            }

            // Instantiate WebFile; caller will be responsible for calling xClose() on it, which
            // will make it self-delete.
            auto webfile = new File(uri, filename, file_size, std::move(curlpool), log_level);
            webfile->InitHandle(pFile);
            // initiate prefetch of first 64KiB
            *pOutFlags = flags;
            return SQLITE_OK;
        } catch (std::bad_alloc &) {
            return SQLITE_IOERR_NOMEM;
        }
    }

    int GetLastError(int nByte, char *zErrMsg) override {
        if (nByte && last_error_.size()) {
            strncpy(zErrMsg, last_error_.c_str(), nByte);
            zErrMsg[nByte - 1] = 0;
            return SQLITE_OK;
        }
        return SQLiteVFS::Wrapper::GetLastError(nByte, zErrMsg);
    }

    std::string FileNameForLog(const std::string &uri) {
        std::string ans = uri;
        auto p = ans.find('?');
        if (p != std::string::npos) {
            ans = ans.substr(0, p);
        }
        p = ans.rfind('/');
        if (p != std::string::npos) {
            ans = ans.substr(p + 1);
        }
        if (ans.size() > 97) {
            ans = ans.substr(0, 97) + "...";
        }
        return ans;
    }
};

} // namespace WebVFS
