# MPark.Variant

> __C++17__ `std::variant` for __C++11__/__14__/__17__

[![release][badge.release]][release]
[![header][badge.header]][header]
[![travis][badge.travis]][travis]
[![appveyor][badge.appveyor]][appveyor]
[![license][badge.license]][license]
[![godbolt][badge.godbolt]][godbolt]
[![wandbox][badge.wandbox]][wandbox]

[badge.release]: https://img.shields.io/github/release/mpark/variant.svg
[badge.header]: https://img.shields.io/badge/single%20header-master-blue.svg
[badge.travis]: https://travis-ci.org/mpark/variant.svg?branch=master
[badge.appveyor]: https://ci.appveyor.com/api/projects/status/github/mpark/variant?branch=master&svg=true
[badge.license]: https://img.shields.io/badge/license-boost-blue.svg
[badge.godbolt]: https://img.shields.io/badge/try%20it-on%20godbolt-222266.svg
[badge.wandbox]: https://img.shields.io/badge/try%20it-on%20wandbox-5cb85c.svg

[release]: https://github.com/mpark/variant/releases/latest
[header]: https://github.com/mpark/variant/blob/single-header/master/variant.hpp
[travis]: https://travis-ci.org/mpark/variant
[appveyor]: https://ci.appveyor.com/project/mpark/variant
[license]: https://github.com/mpark/variant/blob/master/LICENSE.md
[godbolt]: https://godbolt.org/g/1qYDAK
[wandbox]: https://wandbox.org/permlink/QV3gZ2KQQNwgoFIB

## Introduction

__MPark.Variant__ is an implementation of __C++17__ `std::variant` for __C++11__/__14__/__17__.

  - Based on [my implementation of `std::variant` for __libc++__][libcxx-impl]
  - Continuously tested against __libc++__'s `std::variant` test suite.

[libcxx-impl]: https://reviews.llvm.org/rL288547

## Documentation

  - [cppreference.com](http://en.cppreference.com/w/cpp/utility/variant)
  - [eel.is/c++draft](http://eel.is/c++draft/variant)

## Integration

### Single Header

The [single-header] branch provides a standalone `variant.hpp`
file for each [release](https://github.com/mpark/variant/releases).
Copy it and `#include` away!

[single-header]: https://github.com/mpark/variant/tree/single-header

### Submodule

You can add `mpark/variant` as a submodule to your project.

```bash
git submodule add https://github.com/mpark/variant.git 3rdparty/variant
```

Add the `include` directory to your include path with
`-I3rdparty/variant/include` then `#include` the `variant.hpp` header
with `#include <mpark/variant.hpp>`.

If you use CMake, you can simply use `add_subdirectory(3rdparty/variant)`:

```cmake
cmake_minimum_required(VERSION 3.6.3)

project(HelloWorld CXX)

add_subdirectory(3rdparty/variant)

add_executable(hello-world hello_world.cpp)
target_link_libraries(hello-world mpark_variant)
```

### Installation / CMake `find_package`

```bash
git clone https://github.com/mpark/variant.git
mkdir variant/build && cd variant/build
cmake ..
cmake --build . --target install
```

This will install `mpark/variant` to the default install-directory for
your platform (`/usr/local` for Unix, `C:\Program Files` for Windows).
You can also install at a custom location via the `CMAKE_INSTALL_PREFIX`
variable, (e.g., `cmake .. -DCMAKE_INSTALL_PREFIX=/opt`).

The installed `mpark/variant` can then be found by CMake via `find_package`:

```cmake
cmake_minimum_required(VERSION 3.6.3)

project(HelloWorld CXX)

find_package(mpark_variant 1.3.0 REQUIRED)

add_executable(hello-world hello_world.cpp)
target_link_libraries(hello-world mpark_variant)
```

CMake will search for `mpark/variant` in its default set of
installation prefixes. If `mpark/variant` is installed in
a custom location via the `CMAKE_INSTALL_PREFIX` variable,
you'll likely need to use the `CMAKE_PREFIX_PATH` to specify
the location (e.g., `cmake .. -DCMAKE_PREFIX_PATH=/opt`).

## Requirements

This library requires a standard conformant __C++11__ compiler.
The following compilers are continously tested:

| Compiler                               | Operating System                            | Version String                                                                     |
| -------------------------------------- | ------------------------------------------- | ---------------------------------------------------------------------------------- |
| GCC 4.8.5                              | Ubuntu 16.04.5 LTS                          | g++-4.8 (Ubuntu 4.8.5-4ubuntu8~16.04.1) 4.8.5                                      |
| GCC 4.9.4                              | Ubuntu 16.04.5 LTS                          | g++-4.9 (Ubuntu 4.9.4-2ubuntu1~16.04) 4.9.4                                        |
| GCC 5.5.0                              | Ubuntu 16.04.5 LTS                          | g++-5 (Ubuntu 5.5.0-12ubuntu1~16.04) 5.5.0 20171010                                |
| GCC 6.5.0                              | Ubuntu 16.04.5 LTS                          | g++-6 (Ubuntu 6.5.0-2ubuntu1~16.04) 6.5.0 20181026                                 |
| GCC 7.4.0                              | Ubuntu 16.04.5 LTS                          | g++-7 (Ubuntu 7.4.0-1ubuntu1\~16.04\~ppa1) 7.4.0                                   |
| GCC 8.1.0                              | Ubuntu 16.04.5 LTS                          | g++-8 (Ubuntu 8.1.0-5ubuntu1~16.04) 8.1.0                                          |
| Clang 3.6.2                            | Ubuntu 16.04.5 LTS                          | Ubuntu clang version 3.6.2-3ubuntu2 (tags/RELEASE_362/final) (based on LLVM 3.6.2) |
| Clang 3.7.1                            | Ubuntu 16.04.5 LTS                          | Ubuntu clang version 3.7.1-2ubuntu2 (tags/RELEASE_371/final) (based on LLVM 3.7.1) |
| Clang 3.8.0                            | Ubuntu 16.04.5 LTS                          | clang version 3.8.0-2ubuntu4 (tags/RELEASE_380/final)                              |
| Clang 3.9.1                            | Ubuntu 16.04.5 LTS                          | clang version 3.9.1-4ubuntu3~16.04.2 (tags/RELEASE_391/rc2)                        |
| Clang 4.0.0                            | Ubuntu 16.04.5 LTS                          | clang version 4.0.0-1ubuntu1~16.04.2 (tags/RELEASE_400/rc1)                        |
| Clang 5.0.0                            | Ubuntu 16.04.5 LTS                          | clang version 5.0.0-3~16.04.1 (tags/RELEASE_500/final)                             |
| Clang 6.0.0                            | Ubuntu 16.04.5 LTS                          | clang version 6.0.0-1ubuntu2~16.04.1 (tags/RELEASE_600/final)                      |
| Clang 7.0.1                            | Ubuntu 16.04.5 LTS                          | clang version 7.0.1-svn347285-1\~exp1\~20181124105320.40 (branches/release_70)     |
| Clang Xcode 7.3                        | Darwin Kernel Version 15.6.0 (OS X 10.11.6) | Apple LLVM version 7.3.0 (clang-703.0.31)                                          |
| Clang Xcode 8.3                        | Darwin Kernel Version 16.6.0 (OS X 10.12.5) | Apple LLVM version 8.1.0 (clang-802.0.42)                                          |
| Clang Xcode 9.4                        | Darwin Kernel Version 17.4.0 (OS X 10.13.3) | Apple LLVM version 9.1.0 (clang-902.0.39.2)                                        |
| Clang Xcode 10.1                       | Darwin Kernel Version 17.7.0 (OS X 10.13.6) | Apple LLVM version 10.0.0 (clang-1000.11.45.5)                                     |
| Visual Studio 14 2015                  | Visual Studio 2015 with Update 3            | MSVC 19.0.24241.7                                                                  |
| Visual Studio 15 2017                  | Visual Studio 2017 with Update 8            | MSVC 19.15.26732.1                                                                 |
| Visual Studio 15 2017                  | Visual Studio 2017 with Update 9            | MSVC 19.16.27025.1                                                                 |
| Visual Studio 15 2017 (__Clang/LLVM__) | Visual Studio 2017                          | Clang 7.0.0                                                                        |

#### NOTES
  - __GCC 4.8__/__4.9__: `constexpr` support is not available for `visit` and relational operators.
  - Enabling __libc++__ `std::variant` tests require `-std=c++17` support.

## CMake Variables

  -  __`MPARK_VARIANT_INCLUDE_TESTS`__:`STRING` (__default__: `""`)

     Semicolon-separated list of tests to build.
     Possible values are `mpark`, and `libc++`.

     __NOTE__: The __libc++__ `std::variant` tests are built with `-std=c++17`.

## Unit Tests

Refer to [test/README.md](test/README.md).

## License

Distributed under the [Boost Software License, Version 1.0](LICENSE.md).
