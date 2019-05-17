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
| Aladdin MD5                | Zlib              |                  | Unknown           |          ✗           |                ✗                |
| [ASIO]                     | BSL-1.0           | 1.13.0           | 9229964dc1        |                      |                ✗                |
| [benchmark]                | Apache-2.0        | 1.4.1            | 1.3.0             |                      |                                 |
| [Boost]                    | BSL-1.0           | 1.70.0           | 1.60.0            |                      |                ✗                |
| [GPerfTools]               | BSD-3-Clause      | 2.7              | 2.5               |                      |                ✗                |
| [ICU4]                     | ICU               | 64.2             | 57.1              |          ✗           |                ✗                |
| [Intel Decimal FP Library] | BSD-3-Clause      | 2.0 Update 2     | 2.0 Update 1      |                      |                ✗                |
| [JSON-Schema-Test-Suite]   | MIT               |                  | 728066f9c5        |                      |                                 |
| [libstemmer]               | BSD-3-Clause      |                  | Unknown           |          ✗           |                ✗                |
| [linenoise]                | BSD-3-Clause      |                  | Unknown + changes |                      |                ✗                |
| [MozJS]                    | MPL-2.0           | ESR 60.6.1       | ESR 45.9.0        |                      |                ✗                |
| [MurmurHash3]              | Public Domain     |                  | Unknown + changes |          ✗           |                ✗                |
| [Pcre]                     | BSD-3-Clause      | 8.43             | 8.42              |                      |                ✗                |
| [S2]                       | Apache-2.0        |                  | Unknown           |          ✗           |                ✗                |
| [scons]                    | MIT               | 3.0.4            | 2.5.0             |                      |                                 |
| [Snappy]                   | BSD-3-Clause      | 1.1.7            | 1.1.3             |          ✗           |                ✗                |
| [sqlite]                   | Public Domain     | 3280000          | 3190300           |          ✗           |                ✗                |
| [timelib]                  | MIT               | 2018.01          | 2018.01alpha1     |                      |                ✗                |
| [TomCrypt]                 | Public Domain     | 1.18.2           | 1.18.1            |          ✗           |                ✗                |
| [Unicode]                  | Unicode-DFS-2015  | 12.0.0           | 8.0.0             |          ✗           |                ✗                |
| [Valgrind]                 | BSD-3-Clause<sup>\[<a href="#note_vg" id="ref_vg">1</a>]</sup> | 3.14.0 | 3.11.0 | |             ✗                |
| [variant]                  | BSL-1.0           | 1.4.0            | 1.3.0             |                      |                ✗                |
| [wiredtiger]               |                   |                  | <sup>\[<a href="#note_wt" id="ref_wt">2</a>]</sup> | ✗ |  ✗                |
| [yaml-cpp]                 | MIT               | 0.6.2            | 0.5.3             |                      |                ✗                |
| [Zlib]                     | Zlib              | 1.2.11           | 1.2.11            |          ✗           |                ✗                |

[ASIO]: https://github.com/chriskohlhoff/asio
[benchmark]: https://github.com/google/benchmark
[Boost]: http://www.boost.org/
[GPerfTools]: https://github.com/gperftools/gperftools
[ICU4]: http://site.icu-project.org/download/
[Intel Decimal FP Library]: https://software.intel.com/en-us/articles/intel-decimal-floating-point-math-library
[JSON-Schema-Test-Suite]: https://github.com/json-schema-org/JSON-Schema-Test-Suite
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
| [assertions](https://github.com/smartystreets/assertions)           |
| [crypto](https://golang.org/x/crypto)                               |
| [escaper](https://github.com/10gen/escaper)                         |
| [gls](https://github.com/jtolds/gls)                                |
| [go-cache](https://github.com/patrickmn/go-cache)                   |
| [go-flags](https://github.com/jessevdk/go-flags)                    |
| [go-runewidth](https://github.com/mattn/go-runewidth)               |
| [go-rootcerts](https://github.com/hashicorp/go-rootcerts)           |
| [goconvey](https://github.com/smartystreets/goconvey)               |
| [gopacket](https://github.com/google/gopacket)                      |
| [gopass](https://github.com/howeyc/gopass)                          |
| [gopherjs](https://github.com/gopherjs/gopherjs)                    |
| [llmgo](https://github.com/10gen/llmgo)                             |
| [mgo](https://github.com/10gen/mgo)                                 |
| [mongo-go-driver](https://github.com/mongodb/mongo-go-driver)       |
| [mongo-lint](https://github.com/3rf/mongo-lint)                     |
| [mongo-tools-common](https://github.com/mongodb/mongo-tools-common) |
| [openssl](https://github.com/10gen/openssl)                         |
| [oglematchers](https://github.com/jacobsa/oglematchers)             |
| [scram](https://github.com/xdg/scram)                               |
| [snappy](https://github.com/golang/snappy)                          |
| [spacelog](https://github.com/spacemonkeygo/spacelog)               |
| [stack](https://github.com/go-stack/stack)                          |
| [stringprep](https://github.com/xdg/stringprep)                     |
| [sync](https://golang.org/x/sync)                                   |
| [termbox-go](https://github.com/nsf/termbox-go)                     |
| [text](https://golang.org/x/text)                                   |
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
