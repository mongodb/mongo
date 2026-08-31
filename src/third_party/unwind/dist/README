# libunwind

[![CI - Unix](https://github.com/libunwind/libunwind/actions/workflows/CI-unix.yml/badge.svg)](https://github.com/libunwind/libunwind/actions/workflows/CI-unix.yml)
[![CI - Windows](https://github.com/libunwind/libunwind/actions/workflows/CI-win.yml/badge.svg)](https://github.com/libunwind/libunwind/actions/workflows/CI-win.yml)

This library supports several architecture/operating-system combinations:

| System  | Architecture | Status |
| :------ | :----------- | :----- |
| Linux   | x86-64       | ✓      |
| Linux   | x86          | ✓      |
| Linux   | ARM          | ✓      |
| Linux   | AArch64      | ✓      |
| Linux   | PPC32        | ✓      |
| Linux   | PPC64        | ✓      |
| Linux   | SuperH       | ✓      |
| Linux   | IA-64        | ✓      |
| Linux   | PARISC       | Works well, but C library missing unwind-info |
| Linux   | MIPS         | ✓      |
| Linux   | RISC-V       | 64-bit only |
| Linux   | LoongArch    | 64-bit only |
| HP-UX   | IA-64        | Mostly works, but known to have serious limitations |
| FreeBSD | x86-64       | ✓      |
| FreeBSD | x86          | ✓      |
| FreeBSD | AArch64      | ✓      |
| FreeBSD | PPC32        | ✓      |
| FreeBSD | PPC64        | ✓      |
| QNX     | Aarch64      | ✓      |
| QNX     | x86-64       | ✓      |
| Solaris | x86-64       | ✓      |

## Libc Requirements

libunwind depends on getcontext(), setcontext() functions which are missing
from C libraries like musl-libc because they are considered to be "obsolescent"
API by POSIX document.  The following table tries to track current status of
such dependencies

 - r, requires
 - p, provides its own implementation
 - empty, no requirement

| Architecture | getcontext | setcontext |
|--------------|------------|------------|
|    aarch64   |     p      |            |
|    arm       |     p      |            |
|    hppa      |     p      |      p     |
|    ia64      |     p      |      r     |
|    loongarch |     p      |            |
|    mips      |     p      |            |
|    ppc32     |     r      |            |
|    ppc64     |     r      |      r     |
|    riscv     |     p      |      p     |
|    s390x     |     p      |      p     |
|    sh        |     r      |            |
|    x86       |     p      |      r     |
|    x86_64    |     p      |      p     |

## General Build Instructions

In general, this library can be built and installed with the following
commands:

    $ autoreconf -i # Needed only for building from git. Depends on libtool.
    $ ./configure --prefix=PREFIX
    $ make
    $ make install

where `PREFIX` is the installation prefix.  By default, a prefix of
`/usr/local` is used, such that `libunwind.a` is installed in
`/usr/local/lib` and `unwind.h` is installed in `/usr/local/include`.  For
testing, you may want to use a prefix of `/usr/local` instead.


### Building with Intel compiler

#### Version 8 and later

Starting with version 8, the preferred name for the IA-64 Intel
compiler is `icc` (same name as on x86).  Thus, the configure-line
should look like this:

    $ ./configure CC=icc CFLAGS="-g -O3 -ip" CXX=icc CCAS=gcc CCASFLAGS=-g \
        LDFLAGS="-L$PWD/src/.libs"


### Building on HP-UX

For the time being, libunwind must be built with GCC on HP-UX.

libunwind should be configured and installed on HP-UX like this:

    $ ./configure CFLAGS="-g -O2 -mlp64" CXXFLAGS="-g -O2 -mlp64"

Caveat: Unwinding of 32-bit (ILP32) binaries is not supported at the moment.

### Building for PowerPC64 / Linux

For building for power64 you should use:

    $ ./configure CFLAGS="-g -O2 -m64" CXXFLAGS="-g -O2 -m64"

If your power support altivec registers:

    $ ./configure CFLAGS="-g -O2 -m64 -maltivec" CXXFLAGS="-g -O2 -m64 -maltivec"

To check if your processor has support for vector registers (altivec):

    cat /proc/cpuinfo | grep altivec

and should have something like this:

    cpu             : PPC970, altivec supported

If libunwind seems to not work (backtracing failing), try to compile
it with `-O0`, without optimizations. There are some compiler problems
depending on the version of your gcc.

### Building on FreeBSD

General building instructions apply. To build and execute several tests
on older versions of FreeBSD, you need libexecinfo library available in
ports as devel/libexecinfo. This port has been removed as of 2017 and is
indeed no longer needed.

## Regression Testing

After building the library, you can run a set of regression tests with:

    $ make check

### Expected results on x86 Linux

The following tests are expected to fail on x86 Linux:

* `test-ptrace`

### Expected results on x86-64 Linux

The following tests are expected to fail on x86-64 Linux:

* `run-ptrace-misc` (see <http://gcc.gnu.org/bugzilla/show_bug.cgi?id=18748>
  and <http://gcc.gnu.org/bugzilla/show_bug.cgi?id=18749>)

### Expected results on PARISC Linux

The following tests are expected to fail on x86-64 Linux:

* `Gtest-bt` (backtrace truncated at `kill()` due to lack of unwind-info)
* `Ltest-bt` (likewise)
* `Gtest-resume-sig` (`Gresume.c:my_rt_sigreturn()` is wrong somehow)
* `Ltest-resume-sig` (likewise)
* `Gtest-init` (likewise)
* `Ltest-init` (likewise)
* `Gtest-dyn1` (no dynamic unwind info support yet)
* `Ltest-dyn1` (no dynamic unwind info support yet)
* `test-setjmp` (`longjmp()` not implemented yet)
* `run-check-namespace` (toolchain doesn't support `HIDDEN` yet)

### Expected results on HP-UX

`make check` is currently unsupported for HP-UX.  You can try to run
it, but most tests will fail (and some may fail to terminate).  The
only test programs that are known to work at this time are:

* `tests/bt`
* `tests/Gperf-simple`
* `tests/test-proc-info`
* `tests/test-static-link`
* `tests/Gtest-init`
* `tests/Ltest-init`
* `tests/Gtest-resume-sig`
* `tests/Ltest-resume-sig`

### Expected results on PPC64 Linux

`make check` should run with no more than 10 out of 24 tests failed.

### Expected results on Solaris x86-64

`make check` is passing 27 out of 33 tests. The following six tests are consistently
failing:

* `Gtest-concurrent`
* `Ltest-concurrent`
* `Ltest-init-local-signal`
* `Lrs-race`
* `test-setjmp`
* `x64-unwind-badjmp-signal-frame`

## Performance Testing

This distribution includes a few simple performance tests which give
some idea of the basic cost of various libunwind operations.  After
building the library, you can run these tests with the following
commands:

    $ cd tests
    $ make perf

## Contacting the Developers

Please raise issues and pull requests through the GitHub repository:
<https://github.com/libunwind/libunwind>.
