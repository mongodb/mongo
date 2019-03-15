# MongoDB Third Party Dependencies

MongoDB depends on third party libraries to implement some
functionality. This document describes which libraries are depended
upon, and how. It is maintained by and for humans, and so while it is a
best effort attempt to describe the server’s dependencies, it is subject
to change as libraries are added or removed.

## Server Vendored Libraries

This is the list of third party libraries vendored into the server
codebase, and the upstream source where updates may be obtained. These
sources are periodically consulted, and the existence of new versions is
reflected in this list. A ticket is filed in Jira if a determination is
made to upgrade a vendored library.

Whenever a vendored library is included in released binary artifacts, is
not authored by MongoDB, and has a license which requires reproduction,
a notice will be included in
`THIRD-PARTY-NOTICES`.

| Name                                                                                                            | License           | Upstream Version | Vendored Version                         | Emits persisted data | Distributed in Release Binaries |
| --------------------------------------------------------------------------------------------------------------- | ----------------- | ---------------- | ---------------------------------------- | :------------------: | :-----------------------------: |
| [abseil-cpp](https://github.com/abseil/abseil-cpp)                                                              | Apache-2.0        | HEAD             | 070f6e47b33a2909d039e620c873204f78809492 |                      |                ✗                |
| Aladdin MD5                                                                                                     | Zlib              | Unknown          | Unknown                                  |          ✗           |                ✗                |
| [ASIO](https://github.com/chriskohlhoff/asio)                                                                   | BSL-1.0           | HEAD             | b0926b61b057ce563241d609cae5768ed3a4e1b1 |                      |                ✗                |
| [benchmark](https://github.com/google/benchmark)                                                                | Apache-2.0        | 1.4.1            | 1.4.1                                    |                      |                                 |
| [Boost](http://www.boost.org/)                                                                                  | BSL-1.0           | 1.69.0           | 1.69.0                                   |                      |                ✗                |
| [GPerfTools](https://github.com/gperftools/gperftools)                                                          | BSD-3-Clause      | 2.7              | 2.5, 2.7                                 |                      |                ✗                |
| [ICU4](http://site.icu-project.org/download/)                                                                   | ICU               | 63.1             | 57.1                                     |          ✗           |                ✗                |
| [Intel Decimal FP Library](https://software.intel.com/en-us/articles/intel-decimal-floating-point-math-library) | BSD-3-Clause      | 2.0 Update 2     | 2.0 Update 1                             |                      |                ✗                |
| [JSON-Schema-Test-Suite](https://github.com/json-schema-org/JSON-Schema-Test-Suite)                             | MIT               | HEAD             | 728066f9c5c258ba3                        |                      |                                 |
| [kms-message](https://github.com/mongodb-labs/kms-message)                                                      |                   | HEAD             | 8d91fa28cf179be591f595ca6611f74443357fdb |                      |                ✗                |
| [libstemmer](https://github.com/snowballstem/snowball)                                                          | BSD-3-Clause      | HEAD             | Unknown                                  |          ✗           |                ✗                |
| [linenoise](https://github.com/antirez/linenoise)                                                               | BSD-3-Clause      | HEAD             | Unknown + changes                        |                      |                ✗                |
| [MozJS](https://www.mozilla.org/en-US/security/known-vulnerabilities/firefox-esr)                               | MPL-2.0           | ESR 60.5.1       | ESR 60.3.0                               |                      |                ✗                |
| [MurmurHash3](https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp)                             | Public Domain     | HEAD             | Unknown + changes                        |          ✗           |                ✗                |
| [Pcre](http://www.pcre.org/)                                                                                    | BSD-3-Clause      | 8.43             | 8.42                                     |                      |                ✗                |
| [S2](https://github.com/google/s2geometry)                                                                      | Apache-2.0        | HEAD             | Unknown                                  |          ✗           |                ✗                |
| [scons](https://github.com/SCons/scons)                                                                         | MIT               | 3.0.4            | 3.0.4                                    |                      |                                 |
| [Snappy](https://github.com/google/snappy/releases)                                                             | BSD-3-Clause      | 1.1.7            | 1.1.7                                    |          ✗           |                ✗                |
| [sqlite](https://sqlite.org/)                                                                                   | Public Domain     | 3270200          | 3260000                                  |          ✗           |                ✗                |
| [timelib](https://github.com/derickr/timelib)                                                                   | MIT               | 2018.01          | 2018.01                                  |                      |                ✗                |
| [TomCrypt](https://github.com/libtom/libtomcrypt/releases)                                                      | Public Domain     | 1.18.2           | 1.18.2                                   |          ✗           |                ✗                |
| [Unicode](http://www.unicode.org/versions/enumeratedversions.html)                                              | Unicode-DFS-2015  | 12.0.0           | 8.0.0                                    |          ✗           |                ✗                |
| [Valgrind](http://valgrind.org/downloads/current.html)                                                          | BSD-3-Clause\[1\] | 3.14.0           | 3.11.0                                   |                      |                ✗                |
| [variant](https://github.com/mpark/variant)                                                                     | BSL-1.0           | 1.4.0            | 1.3.0                                    |                      |                ✗                |
| [wiredtiger](https://github.com/wiredtiger/wiredtiger)                                                          |                   | HEAD             | \[2\]                                    |          ✗           |                ✗                |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp/releases)                                                         | MIT               | 0.6.2            | 0.6.2                                    |                      |                ✗                |
| [Zlib](https://zlib.net/)                                                                                       | Zlib              | 1.2.11           | 1.2.11                                   |          ✗           |                ✗                |
| [Zstandard](https://github.com/facebook/zstd)                                                                   | BSD-3-Clause      | 1.3.8            | 1.3.7                                    |          ✗           |                ✗                |

## WiredTiger Vendored Test Libraries

The following Python libraries are transitively included by WiredTiger,
and are used by that component for testing. They don’t appear in
released binary artifacts.

| Name            |
| :-------------- |
| concurrencytest |
| discover        |
| extras          |
| python-subunit  |
| testscenarios   |
| testtools       |

## Libraries Imported by Tools

The following Go libraries are vendored into the MongoDB tools. Their
license notices are included in the `THIRD-PARTY-NOTICES.gotools` file.

| Name                                                                |
| :------------------------------------------------------------------ |
| Go (language runtime and JSON/CSV codecs)                           |
| [escaper](https://github.com/10gen/escaper)                         |
| [llmgo](https://github.com/10gen/llmgo)                             |
| [openssl](https://github.com/10gen/openssl)                         |
| [mongo-lint](https://github.com/3rf/mongo-lint)                     |
| [stack](https://github.com/go-stack/stack)                          |
| [snappy](https://github.com/golang/snappy)                          |
| [gopacket](https://github.com/google/gopacket)                      |
| [gopherjs](https://github.com/gopherjs/gopherjs)                    |
| [gopass](https://github.com/howeyc/gopass)                          |
| [go-flags](https://github.com/jessevdk/go-flags)                    |
| [gls](https://github.com/jtolds/gls)                                |
| [go-runewidth](https://github.com/mattn/go-runewidth)               |
| [mongo-go-driver](https://github.com/mongodb/mongo-go-driver)       |
| [mongo-tools-common](https://github.com/mongodb/mongo-tools-common) |
| [termbox-go](https://github.com/nsf/termbox-go)                     |
| [go-cache](https://github.com/patrickmn/go-cache)                   |
| [assertions](https://github.com/smartystreets/assertions)           |
| [goconvey](https://github.com/smartystreets/goconvey)               |
| [spacelog](https://github.com/spacemonkeygo/spacelog)               |
| [scram](https://github.com/xdg/scram)                               |
| [stringprep](https://github.com/xdg/stringprep)                     |
| [crypto](https://golang.org/x/crypto)                               |
| [sync](https://golang.org/x/sync)                                   |
| [text](https://golang.org/x/text)                                   |
| [mgo](https://github.com/10gen/mgo)                                 |
| [tomb](https://gopkg.in/tomb.v2)                                    |

## Dynamically Linked Libraries

Sometimes MongoDB needs to load libraries provided and managed by the
runtime environment. These libraries are not vendored into the MongoDB
source directory, and are not compiled into release artifacts. Because
they are provided by the runtime environment, the precise versions of
these libraries cannot be known in advance. Further, these libraries may
themselves load other libraries. The full set of transitively linked
libraries will depend on the runtime environment, and cannot be outlined
here. On Windows and Mac OS, other libraries and components provided by
the Operating System may be loaded.

For Windows Enterprise, we may ship precompiled DLLs containing some of
these libraries. Releases prepared in this fashion will include a copy
of these libraries’ license in a file named
`THIRD-PARTY-NOTICES.windows`.

| Name       | Enterprise Only | Has Windows DLLS |
| :--------- | :-------------: | :--------------: |
| Cyrus SASL |       Yes       |       Yes        |
| libldap    |       Yes       |        No        |
| net-snmp   |       Yes       |       Yes        |
| OpenSSL    |       No        |     Yes\[2\]     |
| libcurl    |       No        |        No        |

1.  The majority of Valgrind is licensed under the GPL, with the
    exception of a single header file which is licensed under a BSD
    license. This BSD licensed header is the only file from Valgrind
    which is vendored and consumed by MongoDB.

2.  WiredTiger is maintained by MongoDB in a separate repository. As a
    part of our development process, we periodically ingest the latest
    snapshot of that repository.

3.  OpenSSL is only shipped as a dependency of the MongoDB tools written
    in Go. The MongoDB shell and server binaries use Windows’
    cryptography APIs.
