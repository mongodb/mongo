<!-------------------------------------
Copyright (C) 2022 Intel Corporation
SPDX-License-Identifier: MIT
--------------------------------------->
Intel(R) Intelligent Storage Acceleration Library
=================================================

Notices and Disclaimers
-----------------

No license (express or implied, by estoppel or otherwise) to any intellectual
property rights is granted by this document.

Intel disclaims all express and implied warranties, including without
limitation, the implied warranties of merchantability, fitness for a particular
purpose, and non-infringement, as well as any warranty arising from course of
performance, course of dealing, or usage in trade. This document contains
information on products, services and/or processes in development. All
information provided here is subject to change without notice. Contact your
Intel representative to obtain the latest forecast, schedule, specifications and
roadmaps. The products and services described may contain defects or errors
which may cause deviations from published specifications. Current characterized
errata are available on request. Intel, the Intel logo, Intel Atom, Intel Core
and Xeon are trademarks of Intel Corporation in the U.S. and/or other countries.
\*Other names and brands may be claimed as the property of others. Microsoft,
Windows, and the Windows logo are trademarks, or registered trademarks of
Microsoft Corporation in the United States and/or other countries. Java is a
registered trademark of Oracle and/or its affiliates.

© Intel Corporation.

This software and the related documents are Intel copyrighted materials,
and your use of them is governed by the express license under which they
were provided to you ("License"). Unless the License provides
otherwise, you may not use, modify, copy, publish, distribute, disclose
or transmit this software or the related documents without Intel's prior
written permission. This software and the related documents are provided
as is, with no express or implied warranties, other than those that are
expressly stated in the License.


[![Build Status](https://travis-ci.org/01org/isa-l.svg?branch=master)](https://travis-ci.org/01org/isa-l)

ISA-L is a collection of optimized low-level functions targeting storage
applications.  ISA-L includes:
* Erasure codes - Fast block Reed-Solomon type erasure codes for any
  encode/decode matrix in GF(2^8).
* CRC - Fast implementations of cyclic redundancy check.  Six different
  polynomials supported.
  - iscsi32, ieee32, t10dif, ecma64, iso64, jones64.
* Raid - calculate and operate on XOR and P+Q parity found in common RAID
  implementations.
* Compression - Fast deflate-compatible data compression.
* De-compression - Fast inflate-compatible data compression.

Also see:
* [ISA-L for updates](https://github.com/01org/isa-l).
* For crypto functions see [isa-l_crypto on github](https://github.com/01org/isa-l_crypto).
* The [github wiki](https://github.com/01org/isa-l/wiki) including a list of
  [distros/ports](https://github.com/01org/isa-l/wiki/Ports--Repos) offering binary packages.
* ISA-L [mailing list](https://lists.01.org/mailman/listinfo/isal).
* [Contributing](CONTRIBUTING.md).

Building ISA-L
--------------

### Prerequisites

* Assembler: nasm v2.11.01 or later (nasm v2.13 or better suggested for building in AVX512 support)
  or yasm version 1.2.0 or later.
* Compiler: gcc, clang, icc or VC compiler.
* Make: GNU 'make' or 'nmake' (Windows).
* Optional: Building with autotools requires autoconf/automake packages.

### Autotools
To build and install the library with autotools it is usually sufficient to run:

    ./autogen.sh
    ./configure
    make
    sudo make install

### Makefile
To use a standard makefile run:

    make -f Makefile.unx

### Windows
On Windows use nmake to build dll and static lib:

    nmake -f Makefile.nmake

### Other make targets
Other targets include:
* `make check` : create and run tests
* `make tests` : create additional unit tests
* `make perfs` : create included performance tests
* `make ex`    : build examples
* `make other` : build other utilities such as compression file tests
* `make doc`   : build API manual
