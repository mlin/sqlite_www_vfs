# sqlite_web_vfs

**SQLite3 extension for read-only HTTP(S) database access**

This [virtual filesystem extension](https://www.sqlite.org/vfs.html) provides read-only access to database files over HTTP(S), including S3 and the like, without going through a FUSE mount (which is a fine alternative when available).

With the [extension loaded](https://sqlite.org/loadext.html), use the normal SQLite3 API to open the special URI: 

```
file:/__web__?vfs=web&mode=ro&uri={{PERCENT_ENCODED_URL}}
```

where `{{PERCENT_ENCODED_URL}}` is the database file's complete http(s) URL passed through [percent-encoding](https://en.wikipedia.org/wiki/Percent-encoding) (including any percent-encoded query string of its own). The URL server must support HEAD and [GET range](https://developer.mozilla.org/en-US/docs/Web/HTTP/Range_requests) requests, and the content must not change during the session.

SQLite reads from a database file in small pages (default 4 KiB), which would be very inefficient to serve with one-to-one HTTP requests. Instead, the extension heuristically consolidate page reads into larger HTTP requests, and makes concurrent read-ahead requests. This works well for point lookups and queries that scan largely-contiguous slices of tables and indexes (and a modest number thereof). It doesn't work so well for big multi-way joins and other aggressively random access patterns; in those cases, it's better to download the database file upfront and open it locally.

It's a good idea to [VACUUM](https://sqlite.org/lang_vacuum.html) a database file for serving over the web, and to increase the reader's [page cache size](https://www.sqlite.org/pragma.html#pragma_cache_size).

### Build from source

![CI](https://github.com/mlin/sqlite_web_vfs/workflows/CI/badge.svg?branch=main)

Requirements:

* Linux or macOS
* C++11 build system with CMake
* Dev packages for SQLite3 and libcurl
* (Tests only) python3 and pytest

```
cmake -DCMAKE_BUILD_TYPE=Release -B build . && cmake --build build -j8
env -C build ctest -V
```

The extension library is `build/web_vfs.so` or `build/web_vfs.dylib`.
