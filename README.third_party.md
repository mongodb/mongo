[DO NOT MODIFY THIS FILE MANUALLY. It is generated by src/third_party/scripts/gen_thirdpartyreadme.py]: #

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

| Name                                                 | License                                                                                             | Vendored Version                         | Emits persisted data | Distributed in Release Binaries |
| ---------------------------------------------------- | --------------------------------------------------------------------------------------------------- | ---------------------------------------- | -------------------- | ------------------------------- |
| [Abseil]                                             | Apache-2.0                                                                                          | 20250512.1                               |                      | ✗                               |
| [arximboldi/immer]                                   | BSL-1.0                                                                                             | Unknown                                  |                      | ✗                               |
| [Asio C++ Library]                                   | BSL-1.0                                                                                             | 1.12.2                                   |                      | ✗                               |
| [aws-sdk - the AWS SDK client library]               | Apache-2.0                                                                                          | 1.11.471                                 |                      | ✗                               |
| [benchmark]                                          | Apache-2.0                                                                                          | v1.5.2                                   |                      |                                 |
| [Boost C++ Libraries - boost]                        | BSL-1.0                                                                                             | 1.88.0                                   |                      | ✗                               |
| [c-ares]                                             | MIT                                                                                                 | 1.27.0                                   |                      | ✗                               |
| [concurrencytest]                                    | GPL-3.0-or-later                                                                                    | 0.1.2                                    | unknown              |                                 |
| [Cyrus SASL]                                         | BSD-Attribution-HPND-disclaimer                                                                     | 2.1.28                                   | unknown              |                                 |
| [dcleblanc/SafeInt]                                  | MIT                                                                                                 | 3.0.26                                   |                      | ✗                               |
| [derickr/timelib]                                    | MIT                                                                                                 | 2022.13                                  |                      | ✗                               |
| [discover]                                           | BSD-3-Clause                                                                                        | 0.4.0                                    | unknown              |                                 |
| [fmtlib/fmt]                                         | MIT                                                                                                 | 11.1.3                                   |                      | ✗                               |
| [folly]                                              | Apache-2.0                                                                                          | v2025.04.21.00                           |                      | ✗                               |
| [google-re2]                                         | BSD-3-Clause                                                                                        | 2023-11-01                               |                      | ✗                               |
| [google-snappy]                                      | BSD-3-Clause                                                                                        | 1.1.10                                   | ✗                    | ✗                               |
| [google/s2geometry]                                  | Apache-2.0                                                                                          | Unknown                                  | ✗                    | ✗                               |
| [gperftools]                                         | BSD-3-Clause                                                                                        | 2.9.1                                    |                      | ✗                               |
| [grpc]                                               | Apache-2.0                                                                                          | 1.59.5                                   |                      | ✗                               |
| [ICU for C/C++ (ICU4C)]                              | BSD-3-Clause, MIT v2 with Ad Clause License, Public Domain, BSD-2-Clause                            | 57.1                                     | ✗                    | ✗                               |
| [Intel Decimal Floating-Point Math Library]          | BSD-3-Clause                                                                                        | v2.0 U1                                  |                      | ✗                               |
| [jbeder/yaml-cpp]                                    | MIT                                                                                                 | 0.6.3                                    |                      | ✗                               |
| [JSON-Schema-Test-Suite]                             | Unknown License                                                                                     | Unknown                                  |                      |                                 |
| [libmongocrypt]                                      | Apache-2.0                                                                                          | 1.14.0                                   | ✗                    | ✗                               |
| [librdkafka - the Apache Kafka C/C++ client library] | BSD-3-Clause, Xmlproc License, ISC, MIT, Public Domain, Zlib, BSD-2-Clause, Andreas Stolcke License | 2.6.0                                    |                      | ✗                               |
| [LibTomCrypt]                                        | WTFPL, Public Domain                                                                                | 1.18.2                                   | ✗                    | ✗                               |
| [libunwind/libunwind]                                | MIT                                                                                                 | v1.8.1                                   |                      | ✗                               |
| [linenoise]                                          | BSD-2-Clause                                                                                        | Unknown                                  |                      | ✗                               |
| [MongoDB C Driver]                                   | Apache-2.0                                                                                          | 1.28.1                                   | ✗                    | ✗                               |
| [Mozilla Firefox]                                    | MPL-2.0                                                                                             | 128.11.0esr                              | unknown              | ✗                               |
| [nlohmann-json]                                      | MIT                                                                                                 | 3.11.3                                   | ✗                    |                                 |
| [nlohmann.json.decomposed]                           | MIT                                                                                                 | 3.10.5                                   | unknown              |                                 |
| [node]                                               | ISC                                                                                                 | 22.1.0                                   | unknown              |                                 |
| [ocspbuilder]                                        | MIT                                                                                                 | 0.10.2                                   |                      |                                 |
| [ocspresponder]                                      | Apache-2.0                                                                                          | 0.5.0                                    |                      |                                 |
| [opentelemetry-cpp]                                  | Apache-2.0                                                                                          | 1.17                                     | ✗                    |                                 |
| [opentelemetry-proto]                                | Apache-2.0                                                                                          | 1.3.2                                    | ✗                    |                                 |
| [PCRE2]                                              | BSD-3-Clause, Public Domain                                                                         | 10.40                                    |                      | ✗                               |
| [Protobuf]                                           | BSD-3-Clause                                                                                        | v4.25.0                                  |                      | ✗                               |
| [pyiso8601]                                          | MIT                                                                                                 | 2.1.0                                    | unknown              |                                 |
| [RoaringBitmap/CRoaring]                             | Unknown License                                                                                     | v3.0.1                                   |                      | ✗                               |
| [SchemaStore/schemastore]                            | Apache-2.0                                                                                          | Unknown                                  |                      |                                 |
| [smhasher]                                           | Unknown License                                                                                     | Unknown                                  | unknown              | ✗                               |
| [Snowball Stemming Algorithms]                       | BSD-3-Clause                                                                                        | 7b264ffa0f767c579d052fd8142558dc8264d795 | ✗                    | ✗                               |
| [subunit]                                            | BSD-3-Clause, Apache-2.0                                                                            | 1.4.4                                    | unknown              |                                 |
| [tcmalloc]                                           | Apache-2.0                                                                                          | 20230227-snapshot-093ba93c               |                      | ✗                               |
| [testing-cabal/extras]                               | MIT                                                                                                 | 0.0.3                                    | unknown              |                                 |
| [testscenarios]                                      | BSD-3-Clause, Apache-2.0                                                                            | 0.4                                      | unknown              |                                 |
| [testtools]                                          | MIT                                                                                                 | 2.7.1                                    | unknown              |                                 |
| [unicode-data]                                       | Unicode-DFS-2016                                                                                    | 8.0                                      | ✗                    | ✗                               |
| [valgrind]                                           | GPL-2.0-or-later                                                                                    | Unknown                                  |                      | ✗                               |
| [zlib]                                               | Zlib                                                                                                | v1.3.1                                   | ✗                    | ✗                               |
| [zstd]                                               | BSD-3-Clause, GPL-2.0-or-later                                                                      | 1.5.5                                    | ✗                    | ✗                               |

[Abseil]: https://github.com/abseil/abseil-cpp
[Asio C++ Library]: https://github.com/chriskohlhoff/asio
[Boost C++ Libraries - boost]: http://www.boost.org/
[Cyrus SASL]: https://www.cyrusimap.org/sasl/
[ICU for C/C++ (ICU4C)]: http://site.icu-project.org/download/
[Intel Decimal Floating-Point Math Library]: https://software.intel.com/en-us/articles/intel-decimal-floating-point-math-library
[JSON-Schema-Test-Suite]: https://github.com/json-schema-org/JSON-Schema-Test-Suite
[LibTomCrypt]: https://github.com/libtom/libtomcrypt/releases
[MongoDB C Driver]: https://github.com/mongodb/mongo-c-driver
[Mozilla Firefox]: https://www.mozilla.org/en-US/security/known-vulnerabilities/firefox-esr
[PCRE2]: http://www.pcre.org/
[Protobuf]: https://github.com/protocolbuffers/protobuf
[RoaringBitmap/CRoaring]: https://github.com/RoaringBitmap/CRoaring
[SchemaStore/schemastore]: https://www.schemastore.org/json/
[Snowball Stemming Algorithms]: https://github.com/snowballstem/snowball
[arximboldi/immer]: https://github.com/arximboldi/immer
[aws-sdk - the AWS SDK client library]: https://github.com/aws/aws-sdk-cpp
[benchmark]: https://github.com/google/benchmark
[c-ares]: https://c-ares.org/
[concurrencytest]: https://pypi.org/project/concurrencytest/
[dcleblanc/SafeInt]: https://github.com/dcleblanc/SafeInt
[derickr/timelib]: https://github.com/derickr/timelib
[discover]: https://pypi.org/project/discover/
[fmtlib/fmt]: http://fmtlib.net/
[folly]: https://github.com/facebook/folly
[google-re2]: https://github.com/google/re2
[google-snappy]: https://github.com/google/snappy/releases
[google/s2geometry]: https://github.com/google/s2geometry
[gperftools]: https://github.com/gperftools/gperftools
[grpc]: https://github.com/grpc/grpc
[jbeder/yaml-cpp]: https://github.com/jbeder/yaml-cpp/releases
[libmongocrypt]: https://github.com/mongodb/libmongocrypt
[librdkafka - the Apache Kafka C/C++ client library]: https://github.com/confluentinc/librdkafka
[libunwind/libunwind]: http://www.github.com/libunwind/libunwind
[linenoise]: https://github.com/antirez/linenoise
[nlohmann-json]: https://github.com/open-telemetry/opentelemetry-proto
[nlohmann.json.decomposed]: https://www.nuget.org/packages/nlohmann.json.decomposed
[node]: https://nodejs.org/en/blog/release
[ocspbuilder]: https://github.com/wbond/ocspbuilder
[ocspresponder]: https://github.com/threema-ch/ocspresponder
[opentelemetry-cpp]: https://github.com/open-telemetry/opentelemetry-cpp/
[opentelemetry-proto]: https://github.com/open-telemetry/opentelemetry-proto
[pyiso8601]: https://pypi.org/project/iso8601/
[smhasher]: https://github.com/aappleby/smhasher/blob/a6bd3ce/
[subunit]: https://github.com/testing-cabal/subunit
[tcmalloc]: https://github.com/google/tcmalloc
[testing-cabal/extras]: https://github.com/testing-cabal/extras
[testscenarios]: https://pypi.org/project/testscenarios/
[testtools]: https://github.com/testing-cabal/testtools
[unicode-data]: http://www.unicode.org/versions/enumeratedversions.html
[valgrind]: http://valgrind.org/downloads/current.html
[zlib]: https://zlib.net/
[zstd]: https://github.com/facebook/zstd

## WiredTiger Vendored Test Libraries

The following Python libraries are transitively included by WiredTiger,
and are used by that component for testing. They don't appear in
released binary artifacts.

| Name                     |
| ------------------------ |
| concurrencytest          |
| discover                 |
| nlohmann.json.decomposed |
| pyiso8601                |
| subunit                  |
| testing-cabal/extras     |
| testscenarios            |
| testtools                |

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
