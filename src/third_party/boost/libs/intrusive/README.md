Boost.Intrusive
==========

Boost.Intrusive, part of collection of the [Boost C++ Libraries](http://github.com/boostorg), is a library presenting intrusive containers to the world of C++. Intrusive containers are special containers that offer better performance and exception safety guarantees than non-intrusive containers (like STL containers). The performance benefits of intrusive containers makes them ideal as a building block to efficiently construct complex data structures like multi-index containers or to design high performance code like memory allocation algorithms.

While intrusive containers were and are widely used in C, they became more and more forgotten in C++ due to the presence of the standard containers which don't support intrusive techniques.Boost.Intrusive wants to push intrusive containers usage encapsulating the implementation in STL-like interfaces. Hence anyone familiar with standard containers can easily use Boost.Intrusive.

### License

Distributed under the [Boost Software License, Version 1.0](http://www.boost.org/LICENSE_1_0.txt).

### Properties

* C++03
* Header-Only

### Build Status

Branch          | Travis | Appveyor | Coverity Scan | codecov.io | Deps | Docs | Tests |
:-------------: | ------ | -------- | ------------- | ---------- | ---- | ---- | ----- |
[`master`](https://github.com/boostorg/intrusive/tree/master) | [![Build Status](https://travis-ci.org/boostorg/intrusive.svg?branch=master)](https://travis-ci.org/boostorg/intrusive) | [![Build status](https://ci.appveyor.com/api/projects/status/9ckrveolxsonxfnb/branch/master?svg=true)](https://ci.appveyor.com/project/jeking3/intrusive-0k1xg/branch/master) | [![Coverity Scan Build Status](https://scan.coverity.com/projects/16048/badge.svg)](https://scan.coverity.com/projects/boostorg-intrusive) | [![codecov](https://codecov.io/gh/boostorg/intrusive/branch/master/graph/badge.svg)](https://codecov.io/gh/boostorg/intrusive/branch/master)| [![Deps](https://img.shields.io/badge/deps-master-brightgreen.svg)](https://pdimov.github.io/boostdep-report/master/intrusive.html) | [![Documentation](https://img.shields.io/badge/docs-master-brightgreen.svg)](http://www.boost.org/doc/libs/master/doc/html/intrusive.html) | [![Enter the Matrix](https://img.shields.io/badge/matrix-master-brightgreen.svg)](http://www.boost.org/development/tests/master/developer/intrusive.html)
[`develop`](https://github.com/boostorg/intrusive/tree/develop) | [![Build Status](https://travis-ci.org/boostorg/intrusive.svg?branch=develop)](https://travis-ci.org/boostorg/intrusive) | [![Build status](https://ci.appveyor.com/api/projects/status/9ckrveolxsonxfnb/branch/develop?svg=true)](https://ci.appveyor.com/project/jeking3/intrusive-0k1xg/branch/develop) | [![Coverity Scan Build Status](https://scan.coverity.com/projects/16048/badge.svg)](https://scan.coverity.com/projects/boostorg-intrusive) | [![codecov](https://codecov.io/gh/boostorg/intrusive/branch/develop/graph/badge.svg)](https://codecov.io/gh/boostorg/intrusive/branch/develop) | [![Deps](https://img.shields.io/badge/deps-develop-brightgreen.svg)](https://pdimov.github.io/boostdep-report/develop/intrusive.html) | [![Documentation](https://img.shields.io/badge/docs-develop-brightgreen.svg)](http://www.boost.org/doc/libs/develop/doc/html/intrusive.html) | [![Enter the Matrix](https://img.shields.io/badge/matrix-develop-brightgreen.svg)](http://www.boost.org/development/tests/develop/developer/intrusive.html)

### Directories

| Name        | Purpose                        |
| ----------- | ------------------------------ |
| `doc`       | documentation                  |
| `example`   | examples                       |
| `include`   | headers                        |
| `proj`      | ide projects                   |
| `test`      | unit tests                     |

### More information

* [Ask questions](http://stackoverflow.com/questions/ask?tags=c%2B%2B,boost,boost-intrusive)
* [Report bugs](https://github.com/boostorg/intrusive/issues): Be sure to mention Boost version, platform and compiler you're using. A small compilable code sample to reproduce the problem is always good as well.
* Submit your patches as pull requests against **develop** branch. Note that by submitting patches you agree to license your modifications under the [Boost Software License, Version 1.0](http://www.boost.org/LICENSE_1_0.txt).
* Discussions about the library are held on the [Boost developers mailing list](http://www.boost.org/community/groups.html#main). Be sure to read the [discussion policy](http://www.boost.org/community/policy.html) before posting and add the `[intrusive]` tag at the beginning of the subject line.

