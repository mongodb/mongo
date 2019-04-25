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

| Name                       | License           | Upstream Version | Vendored Version  | Emits persisted data | Distributed in Release Binaries |
| ---------------------------| ----------------- | ---------------- | ------------------| :------------------: | :-----------------------------: |
| [abseil-cpp]               | Apache-2.0        |                  | 070f6e47b3        |                      |                ✗                |
| Aladdin MD5                | Zlib              |                  | Unknown           |          ✗           |                ✗                |
| [ASIO]                     | BSL-1.0           | 1.13.0           | b0926b61b0        |                      |                ✗                |
| [benchmark]                | Apache-2.0        | 1.4.1            | 1.4.1             |                      |                                 |
| [Boost]                    | BSL-1.0           | 1.70.0           | 1.69.0            |                      |                ✗                |
| [fmt]                      | BSD-2-Clause      |                  | 018d8b57f6        |                      |                ✗                |
| [GPerfTools]               | BSD-3-Clause      | 2.7              | 2.7               |                      |                ✗                |
| [ICU4]                     | ICU               | 64.2             | 57.1              |          ✗           |                ✗                |
| [Intel Decimal FP Library] | BSD-3-Clause      | 2.0 Update 2     | 2.0 Update 1      |                      |                ✗                |
| [JSON-Schema-Test-Suite]   | MIT               |                  | 728066f9c5        |                      |                                 |
| [kms-message]              |                   |                  | 75e391a037        |                      |                ✗                |
| [libstemmer]               | BSD-3-Clause      |                  | Unknown           |          ✗           |                ✗                |
| [linenoise]                | BSD-3-Clause      |                  | Unknown + changes |                      |                ✗                |
| [MozJS]                    | MPL-2.0           | ESR 60.6.1       | ESR 60.3.0        |                      |                ✗                |
| [MurmurHash3]              | Public Domain     |                  | Unknown + changes |          ✗           |                ✗                |
| [Pcre]                     | BSD-3-Clause      | 8.43             | 8.42              |                      |                ✗                |
| [S2]                       | Apache-2.0        |                  | Unknown           |          ✗           |                ✗                |
| [scons]                    | MIT               | 3.0.4            | 3.0.4             |                      |                                 |
| [Snappy]                   | BSD-3-Clause      | 1.1.7            | 1.1.7             |          ✗           |                ✗                |
| [sqlite]                   | Public Domain     | 3280000          | 3260000           |          ✗           |                ✗                |
| [timelib]                  | MIT               | 2018.01          | 2018.01           |                      |                ✗                |
| [TomCrypt]                 | Public Domain     | 1.18.2           | 1.18.2            |          ✗           |                ✗                |
| [Unicode]                  | Unicode-DFS-2015  | 12.0.0           | 8.0.0             |          ✗           |                ✗                |
| [Valgrind]                 | BSD-3-Clause<sup>\[<a href="#note_vg" id="ref_vg">1</a>]</sup> | 3.14.0 | 3.11.0 | |             ✗                |
| [variant]                  | BSL-1.0           | 1.4.0            | 1.3.0             |                      |                ✗                |
| [wiredtiger]               |                   |                  | <sup>\[<a href="#note_wt" id="ref_wt">2</a>]</sup> | ✗ |  ✗                |
| [yaml-cpp]                 | MIT               | 0.6.2            | 0.6.2             |                      |                ✗                |
| [Zlib]                     | Zlib              | 1.2.11           | 1.2.11            |          ✗           |                ✗                |
| [Zstandard]                | BSD-3-Clause      | 1.4.0            | 1.3.7             |          ✗           |                ✗                |

[abseil-cpp]: https://github.com/abseil/abseil-cpp
[ASIO]: https://github.com/chriskohlhoff/asio
[benchmark]: https://github.com/google/benchmark
[Boost]: http://www.boost.org/
[fmt]: http://fmtlib.net/
[GPerfTools]: https://github.com/gperftools/gperftools
[ICU4]: http://site.icu-project.org/download/
[Intel Decimal FP Library]: https://software.intel.com/en-us/articles/intel-decimal-floating-point-math-library
[JSON-Schema-Test-Suite]: https://github.com/json-schema-org/JSON-Schema-Test-Suite
[kms-message]: https://github.com/mongodb-labs/kms-message
[libstemmer]: https://github.com/snowballstem/snowball
[linenoise]: https://github.com/antirez/linenoise
[MozJS]: https://www.mozilla.org/en-US/security/known-vulnerabilities/firefox-esr
[MurmurHash3]: https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
[Pcre]: http://www.pcre.org/
[S2]: https://github.com/google/s2geometry
[scons]: https://github.com/SCons/scons
[Snappy]: https://github.com/google/snappy/releases
[sqlite]: https://sqlite.org/
[timelib]: https://github.com/derickr/timelib
[TomCrypt]: https://github.com/libtom/libtomcrypt/releases
[Unicode]: http://www.unicode.org/versions/enumeratedversions.html
[Valgrind]: http://valgrind.org/downloads/current.html
[variant]: https://github.com/mpark/variant
[wiredtiger]: https://github.com/wiredtiger/wiredtiger
[yaml-cpp]: https://github.com/jbeder/yaml-cpp/releases
[Zlib]: https://zlib.net/
[Zstandard]: https://github.com/facebook/zstd

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

| Name       | Enterprise Only | Has Windows DLLs |
| :--------- | :-------------: | :--------------: |
| Cyrus SASL |       Yes       |     Yes          |
| libldap    |       Yes       |     No           |
| net-snmp   |       Yes       |     Yes          |
| OpenSSL    |       No        |     Yes<sup>\[<a href="#note_ssl" id="ref_ssl">3</a>]</sup>    |
| libcurl    |       No        |     No           |


## Notes:

1. <a id="note_vg" href="#ref_vg">^</a>
    The majority of Valgrind is licensed under the GPL, with the exception of a single
    header file which is licensed under a BSD license. This BSD licensed header is the only
    file from Valgrind which is vendored and consumed by MongoDB.

2. <a id="note_wt" href="#ref_wt">^</a>
    WiredTiger is maintained by MongoDB in a separate repository. As a part of our
    development process, we periodically ingest the latest snapshot of that repository.

3. <a id="note_ssl" href="#ref_ssl">^</a>
    OpenSSL is only shipped as a dependency of the MongoDB tools written in Go. The MongoDB
    shell and server binaries use Windows’ cryptography APIs.
