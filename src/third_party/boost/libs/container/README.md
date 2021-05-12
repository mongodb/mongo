Boost.Container
==========

Boost.Container, part of collection of the [Boost C++ Libraries](http://github.com/boostorg), implements several well-known containers, including STL containers. The aim of the library is to offer advanced features not present in standard containers, to offer the latest standard draft features for compilers that don't comply with the latest C++ standard and to offer useful non-STL  containers.

### License

Distributed under the [Boost Software License, Version 1.0](http://www.boost.org/LICENSE_1_0.txt).

### Properties

* C++03
* Mostly header-only, library compilation is required for few features.
* Supports compiler modes without exceptions support (e.g. `-fno-exceptions`).

### Build Status

Branch          | Travis | Appveyor | Coverity Scan | codecov.io | Deps | Docs | Tests |
:-------------: | ------ | -------- | ------------- | ---------- | ---- | ---- | ----- |
[`master`](https://github.com/boostorg/container/tree/master) | [![Build Status](https://travis-ci.org/boostorg/container.svg?branch=master)](https://travis-ci.org/boostorg/container) | [![Build status](https://ci.appveyor.com/api/projects/status/9ckrveolxsonxfnb/branch/master?svg=true)](https://ci.appveyor.com/project/jeking3/container-0k1xg/branch/master) | [![Coverity Scan Build Status](https://scan.coverity.com/projects/16048/badge.svg)](https://scan.coverity.com/projects/boostorg-container) | [![codecov](https://codecov.io/gh/boostorg/container/branch/master/graph/badge.svg)](https://codecov.io/gh/boostorg/container/branch/master)| [![Deps](https://img.shields.io/badge/deps-master-brightgreen.svg)](https://pdimov.github.io/boostdep-report/master/container.html) | [![Documentation](https://img.shields.io/badge/docs-master-brightgreen.svg)](http://www.boost.org/doc/libs/master/doc/html/container.html) | [![Enter the Matrix](https://img.shields.io/badge/matrix-master-brightgreen.svg)](http://www.boost.org/development/tests/master/developer/container.html)
[`develop`](https://github.com/boostorg/container/tree/develop) | [![Build Status](https://travis-ci.org/boostorg/container.svg?branch=develop)](https://travis-ci.org/boostorg/container) | [![Build status](https://ci.appveyor.com/api/projects/status/9ckrveolxsonxfnb/branch/develop?svg=true)](https://ci.appveyor.com/project/jeking3/container-0k1xg/branch/develop) | [![Coverity Scan Build Status](https://scan.coverity.com/projects/16048/badge.svg)](https://scan.coverity.com/projects/boostorg-container) | [![codecov](https://codecov.io/gh/boostorg/container/branch/develop/graph/badge.svg)](https://codecov.io/gh/boostorg/container/branch/develop) | [![Deps](https://img.shields.io/badge/deps-develop-brightgreen.svg)](https://pdimov.github.io/boostdep-report/develop/container.html) | [![Documentation](https://img.shields.io/badge/docs-develop-brightgreen.svg)](http://www.boost.org/doc/libs/develop/doc/html/container.html) | [![Enter the Matrix](https://img.shields.io/badge/matrix-develop-brightgreen.svg)](http://www.boost.org/development/tests/develop/developer/container.html)

### Directories

| Name        | Purpose                        |
| ----------- | ------------------------------ |
| `doc`       | documentation                  |
| `example`   | examples                       |
| `include`   | headers                        |
| `proj`      | ide projects                   |
| `test`      | unit tests                     |

### More information

* [Ask questions](http://stackoverflow.com/questions/ask?tags=c%2B%2B,boost,boost-container)
* [Report bugs](https://github.com/boostorg/container/issues): Be sure to mention Boost version, platform and compiler you're using. A small compilable code sample to reproduce the problem is always good as well.
* Submit your patches as pull requests against **develop** branch. Note that by submitting patches you agree to license your modifications under the [Boost Software License, Version 1.0](http://www.boost.org/LICENSE_1_0.txt).
* Discussions about the library are held on the [Boost developers mailing list](http://www.boost.org/community/groups.html#main). Be sure to read the [discussion policy](http://www.boost.org/community/policy.html) before posting and add the `[container]` tag at the beginning of the subject line.

