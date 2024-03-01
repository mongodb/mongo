# MongoDB Third Party Dependencies

MongoDB depends on third party libraries to implement some
functionality. This document describes which libraries are depended
upon, and how. It is maintained by and for humans, and so while it is a
best effort attempt to describe the server's dependencies, it is subject
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

| Name                       | License                                                        | Vendored Version                                   | Emits persisted data | Distributed in Release Binaries |
| -------------------------- | -------------------------------------------------------------- | -------------------------------------------------- | :------------------: | :-----------------------------: |
| [abseil-cpp]               | Apache-2.0                                                     | 20230802.1                                         |                      |                ✗                |
| [Aladdin MD5]              | Zlib                                                           | Unknown                                            |          ✗           |                ✗                |
| [ASIO]                     | BSL-1.0                                                        | 1.12.2                                             |                      |                ✗                |
| [benchmark]                | Apache-2.0                                                     | 1.5.2                                              |                      |                                 |
| [Boost]                    | BSL-1.0                                                        | 1.79.0                                             |                      |                ✗                |
| [c-ares]                   | MIT                                                            | 1.19.1                                             |                      |                ✗                |
| [double-conversion]        | BSD 3-Clause                                                   | bf4607277fa7133825cb7899015374917cd06b8f           |                      |                ✗                |
| [fmt]                      | BSD-2-Clause                                                   | 7.1.3                                              |                      |                ✗                |
| [GPerfTools]               | BSD-3-Clause                                                   | 2.9.1                                              |                      |                ✗                |
| [gRPC]                     | Apache-2.0                                                     | 1.59.2                                             |                      |                ✗                |
| [ICU4]                     | ICU                                                            | 57.1                                               |          ✗           |                ✗                |
| [immer]                    | BSL-1.0                                                        | d98a68c + changes                                  |                      |                ✗                |
| [Intel Decimal FP Library] | BSD-3-Clause                                                   | 2.0 Update 1                                       |                      |                ✗                |
| [JSON-Schema-Test-Suite]   | MIT                                                            | 728066f9c5                                         |                      |                                 |
| [libstemmer]               | BSD-3-Clause                                                   | Unknown                                            |          ✗           |                ✗                |
| [librdkafka]               | BSD-2-Clause                                                   | 2.0.2                                              |                      |                                 |
| [libmongocrypt]            | Apache-2.0                                                     | 1.8.4                                              |          ✗           |                ✗                |
| [linenoise]                | BSD-3-Clause                                                   | 6cdc775 + changes                                  |                      |                ✗                |
| [mongo-c-driver]           | Apache-2.0                                                     | 1.23.0                                             |          ✗           |                ✗                |
| [MozJS]                    | MPL-2.0                                                        | ESR 91.3.0                                         |                      |                ✗                |
| [MurmurHash3]              | Public Domain                                                  | a6bd3ce + changes                                  |          ✗           |                ✗                |
| [ocspbuilder]              | MIT                                                            | 0.10.2                                             |                      |                                 |
| [ocspresponder]            | Apache-2.0                                                     | 0.5.0                                              |                      |                                 |
| [pcre2]                    | BSD-3-Clause                                                   | 10.40                                              |                      |                ✗                |
| [protobuf]                 | BSD-3-Clause                                                   | 4.25.0                                             |                      |                ✗                |
| [re2]                      | BSD-3-Clause                                                   | 2021-09-01                                         |                      |                ✗                |
| [S2]                       | Apache-2.0                                                     | c872048da5d1 + changes                             |          ✗           |                ✗                |
| [SafeInt]                  | MIT                                                            | 3.0.26                                             |                      |                                 |
| [schemastore.org]          | Apache-2.0                                                     | 6847cfc3a1                                         |                      |                                 |
| [scons]                    | MIT                                                            | 3.1.2                                              |                      |                                 |
| [Snappy]                   | BSD-3-Clause                                                   | 1.1.10                                             |          ✗           |                ✗                |
| [TCMalloc]                 | Apache-2.0                                                     | 093ba93 + changes                                  |                      |                ✗                |
| [timelib]                  | MIT                                                            | 2022.04                                            |                      |                ✗                |
| [TomCrypt]                 | Public Domain                                                  | 1.18.2                                             |          ✗           |                ✗                |
| [Unicode]                  | Unicode-DFS-2015                                               | 8.0.0                                              |          ✗           |                ✗                |
| [libunwind]                | MIT                                                            | 1.6.2 + changes                                    |                      |                ✗                |
| [Valgrind]                 | BSD-4-Clause<sup>\[<a href="#note_vg" id="ref_vg">1</a>]</sup> | 3.17.0                                             |                      |                ✗                |
| [wiredtiger]               |                                                                | <sup>\[<a href="#note_wt" id="ref_wt">2</a>]</sup> |          ✗           |                ✗                |
| [yaml-cpp]                 | MIT                                                            | 0.6.3                                              |                      |                ✗                |
| [Zlib]                     | Zlib                                                           | 1.3                                                |          ✗           |                ✗                |
| [Zstandard]                | BSD-3-Clause                                                   | 1.5.5                                              |          ✗           |                ✗                |

[abseil-cpp]: https://github.com/abseil/abseil-cpp
[ASIO]: https://github.com/chriskohlhoff/asio
[benchmark]: https://github.com/google/benchmark
[Boost]: http://www.boost.org/
[double-conversion]: https://github.com/google/double-conversion "transitive dependency of MozJS"
[fmt]: http://fmtlib.net/
[GPerfTools]: https://github.com/gperftools/gperftools
[gRPC]: https://github.com/grpc/grpc
[ICU4]: http://site.icu-project.org/download/
[immer]: https://github.com/arximboldi/immer
[Intel Decimal FP Library]: https://software.intel.com/en-us/articles/intel-decimal-floating-point-math-library
[JSON-Schema-Test-Suite]: https://github.com/json-schema-org/JSON-Schema-Test-Suite
[libstemmer]: https://github.com/snowballstem/snowball
[librdkafka]: https://github.com/confluentinc/librdkafka
[libmongocrypt]: https://github.com/mongodb/libmongocrypt
[linenoise]: https://github.com/antirez/linenoise
[mongo-c-driver]: https://github.com/mongodb/mongo-c-driver
[MozJS]: https://www.mozilla.org/en-US/security/known-vulnerabilities/firefox-esr
[MurmurHash3]: https://github.com/aappleby/smhasher/blob/a6bd3ce/
[ocspbuilder]: https://github.com/wbond/ocspbuilder
[ocspresponder]: https://github.com/threema-ch/ocspresponder
[pcre2]: http://www.pcre.org/
[protobuf]: https://github.com/protocolbuffers/protobuf
[S2]: https://github.com/google/s2geometry
[SafeInt]: https://github.com/dcleblanc/SafeInt
[schemastore.org]: https://www.schemastore.org/json/
[scons]: https://github.com/SCons/scons
[Snappy]: https://github.com/google/snappy/releases
[TCMalloc]: https://github.com/google/tcmalloc
[timelib]: https://github.com/derickr/timelib
[TomCrypt]: https://github.com/libtom/libtomcrypt/releases
[Unicode]: http://www.unicode.org/versions/enumeratedversions.html
[libunwind]: http://www.nongnu.org/libunwind/
[Valgrind]: http://valgrind.org/downloads/current.html
[wiredtiger]: https://github.com/wiredtiger/wiredtiger
[yaml-cpp]: https://github.com/jbeder/yaml-cpp/releases
[Zlib]: https://zlib.net/
[Zstandard]: https://github.com/facebook/zstd

## WiredTiger Vendored Test Libraries

The following Python libraries are transitively included by WiredTiger,
and are used by that component for testing. They don't appear in
released binary artifacts.

| Name            |
| :-------------- |
| concurrencytest |
| discover        |
| extras          |
| python-subunit  |
| testscenarios   |
| testtools       |

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
of these libraries' license in a file named
`THIRD-PARTY-NOTICES.windows`.

| Name       | Enterprise Only |                    Has Windows DLLs                     |
| :--------- | :-------------: | :-----------------------------------------------------: |
| Cyrus SASL |       Yes       |                           Yes                           |
| libldap    |       Yes       |                           No                            |
| net-snmp   |       Yes       |                           Yes                           |
| OpenSSL    |       No        | Yes<sup>\[<a href="#note_ssl" id="ref_ssl">3</a>]</sup> |
| libcurl    |       No        |                           No                            |

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
   shell and server binaries use Windows' cryptography APIs.
