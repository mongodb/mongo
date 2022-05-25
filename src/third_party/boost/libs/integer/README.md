# Boost.Integer

Boost.Integer, part of collection of the [Boost C++ Libraries](https://github.com/boostorg), provides
integer type support, particularly helpful in generic programming. It provides the means to select
an integer type based upon its properties, like the number of bits or the maximum supported value,
as well as compile-time bit mask selection. There is a derivative of `std::numeric_limits` that provides
integral constant expressions for `min` and `max`...
Finally, it provides two compile-time algorithms: determining the highest power of two in a
compile-time value; and computing min and max of constant expressions.

### Directories

* **doc** - QuickBook documentation sources
* **include** - Interface headers of Boost.Integer
* **test** - Boost.Integer unit tests

### More information

* [Documentation](https://boost.org/libs/integer)
* [Report bugs](https://github.com/boostorg/integer/issues/new). Be sure to mention Boost version, platform and compiler you're using. A small compilable code sample to reproduce the problem is always good as well.
* Submit your patches as pull requests against **develop** branch. Note that by submitting patches you agree to license your modifications under the [Boost Software License, Version 1.0](https://www.boost.org/LICENSE_1_0.txt).

### Build status

Branch          | GitHub Actions | Drone | AppVeyor | Test Matrix | Dependencies |
:-------------: | -------------- | ----- | -------- | ----------- | ------------ |
[`master`](https://github.com/boostorg/integer/tree/master) | [![GitHub Actions](https://github.com/boostorg/integer/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/boostorg/integer/actions?query=branch%3Amaster) | [![Drone](https://drone.cpp.al/api/badges/boostorg/integer/status.svg?ref=refs/heads/master)](https://drone.cpp.al/boostorg/integer) | [![AppVeyor](https://ci.appveyor.com/api/projects/status/iugyf5rf51n99g3w/branch/master?svg=true)](https://ci.appveyor.com/project/Lastique/integer/branch/master) | [![Tests](https://img.shields.io/badge/matrix-master-brightgreen.svg)](http://www.boost.org/development/tests/master/developer/integer.html) | [![Dependencies](https://img.shields.io/badge/deps-master-brightgreen.svg)](https://pdimov.github.io/boostdep-report/master/integer.html)
[`develop`](https://github.com/boostorg/integer/tree/develop) | [![GitHub Actions](https://github.com/boostorg/integer/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/boostorg/integer/actions?query=branch%3Adevelop) | [![Drone](https://drone.cpp.al/api/badges/boostorg/integer/status.svg?ref=refs/heads/develop)](https://drone.cpp.al/boostorg/integer) | [![AppVeyor](https://ci.appveyor.com/api/projects/status/iugyf5rf51n99g3w/branch/develop?svg=true)](https://ci.appveyor.com/project/Lastique/integer/branch/develop) | [![Tests](https://img.shields.io/badge/matrix-develop-brightgreen.svg)](http://www.boost.org/development/tests/develop/developer/integer.html) | [![Dependencies](https://img.shields.io/badge/deps-develop-brightgreen.svg)](https://pdimov.github.io/boostdep-report/develop/integer.html)

### License

Distributed under the [Boost Software License, Version 1.0](https://www.boost.org/LICENSE_1_0.txt).
