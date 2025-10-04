# TCMalloc Quickstart

Note: this Quickstart uses Bazel as the official build system for TCMalloc,
which is supported on Linux, and compatible with most major compilers. The
TCMalloc source code assumes you are using Bazel and contains `BUILD.bazel`
files for that purpose.

This document is designed to allow you to get TCMalloc set up as your custom
allocator within a C++ development environment. We recommend that each person
starting development using TCMalloc at least run through this quick tutorial.

## Prerequisites

Running the code within this tutorial requires:

*   A compatible platform (E.g. Linux). Consult the
    [Platforms Guide](platforms.md) for more information.
*   A compatible C++ compiler *supporting at least C++17*. Most major compilers
    are supported.
*   [Git](https://git-scm.com/) for interacting with the Abseil source code
    repository, which is contained on [GitHub](http://github.com). To install
    Git, consult the [Set Up Git](https://help.github.com/articles/set-up-git/)
    guide on GitHub.

Although you are free to use your own build system, most of the documentation
within this guide will assume you are using [Bazel](https://bazel.build/),
version 4.0 or newer.

To download and install Bazel (and any of its dependencies), consult the
[Bazel Installation Guide](https://docs.bazel.build/versions/master/install.html).

## Getting the TCMalloc Code

You can obtain the TCMalloc code from its repository on GitHub:

```
# Change to the directory where you want to create the code repository
$ cd ~
$ mkdir Source; cd Source
$ git clone https://github.com/google/tcmalloc.git
Cloning into 'tcmalloc'...
remote: Total 1935 (delta 1083), reused 1935 (delta 1083)
Receiving objects: 100% (1935/1935), 1.06 MiB | 0 bytes/s, done.
Resolving deltas: 100% (1083/1083), done.
$
```

Git will create the repository within a directory named `tcmalloc`. Navigate
into this directory and run all tests:

```
$ cd tcmalloc
$ bazel test //tcmalloc/...
INFO: Analyzed 112 targets (12 packages loaded, 606 targets configured).
...
INFO: Build completed successfully, 827 total actions
$
```

Congratulations! You've installed TCMalloc

## Running the TCMalloc Hello World

Once you've verified you have TCMalloc installed correctly, you can compile and
run the
[tcmalloc-hello](https://github.com/google/tcmalloc/blob/master/tcmalloc/testing/hello_main.cc)
sample binary to see how TCMalloc is linked into a sample binary. This tiny
project features proper configuration and a simple `hello_main` to demonstrate
how TCMalloc works.

First, build the `tcmalloc/testing:hello_main` target:

```
tcmalloc$ bazel build tcmalloc/testing:hello_main
Extracting Bazel installation...
Starting local Bazel server and connecting to it...
INFO: Analyzed target //tcmalloc/testing:hello_main (31 packages loaded ...
...
INFO: Build completed successfully, 102 total actions
PASSED in 0.1s
tcmalloc$
```

Now, run the compiled program:

```
tcmalloc$ bazel run tcmalloc/testing:hello_main
...
INFO: Found 1 target...
...
INFO: Build completed successfully, 1 total action
Current heap size = 73728 bytes
hello world!
new'd 1073741824 bytes at 0x14ea40000000
Current heap size = 1073816576 bytes
malloc'd 1073741824 bytes at 0x14eac0000000
Current heap size = 2147558400 bytes
$
```

You can inspect this code within
[`tcmalloc/testing/hello_main.cc`](https://github.com/google/tcmalloc/blob/master/tcmalloc/testing/hello_main.cc)

Happy Coding!

## Creating and Running TCMalloc

Now that you've obtained the TCMalloc code and verified that you can build,
test, and run it, you're ready to use it within your own project.

### Linking Your Code to the TCMalloc Repository

First, create (or select) a source code directory for your work. This directory
should generally not be the `tcmalloc` directory itself; instead, you will link
into that repository from your own source directory.

```
# Change to your main development directory and create a new development
# directory. (If you already have a development directory you'd wish to use,
# you can use that.)
$ cd ~/Source
$ mkdir TestProject; cd TestProject
```

Bazel allows you to link other Bazel projects using `WORKSPACE` files in the
root of your development directories. To add a link to your local TCMalloc
repository within your new project, add the following into a `WORKSPACE` file:

```
local_repository(
  # Name of the TCMalloc repository. This name is defined within your
  # WORKSPACE file, in its `workspace()` metadata
  name = "com_google_tcmalloc",

  # NOTE: Bazel paths must be absolute paths. E.g., you can't use ~/Source
  path = "/PATH_TO_SOURCE/Source/tcmalloc",
)
```

The "name" in the `WORKSPACE` file identifies the name you will use in Bazel
`BUILD` files to refer to the linked repository (in this case
"com_google_tcmalloc").

Note that your path to the TCMalloc source code must be an absolute path.

### Adding Abseil

TCMalloc requires [Abseil](https://abseil.io) which you will also need to
provide as a `local_repository`, or link to a specific commit (we always
recommend the latest commit) using an `http_archive` declaration in the
`WORKSPACE` file:

<pre>
# Abseil HTTP Archive to specific commit
#
# Consult https://github.com/abseil/abseil-cpp/commits/master for the latest
# commit. But DO NOT use master.zip for that purpose. (Sha256 values are not
# stable across master versions.) Click on that specific commit.
#
# Click "Browse Files" on the commit and click on "Clone or Download Code."
#
# Right click on "Download ZIP" to copy the HTTP Archive URL, which you will
# use within the http_archive "urls" field.
#
# Note that you will need to generate a sha256 value for Bazel's http_archive
# to ensure this code is secure. On Linux you can do so with a downloaded .zip
# file using the sha256sum command line:
#
# $ sha256sum github_zip_file.zip
http_archive(
    name = "com_google_absl",
    urls = ["https://github.com/abseil/abseil-cpp/archive/<i>commit_value</i>.zip"],
    strip_prefix = "abseil-cpp-<i>commit_value</i>",
    sha256 = "<i>sha256_of_commit_value</i>",
)
</pre>

### Creating Your Test Code

Within your `TestProject` create an `examples` directory:

```
$ cd TestProject; mkdir examples; cd examples
```

Now, create a `hello_world.cc` C++ file within your `examples` directory:

```
#include <iostream>
#include <cstddef>

int main() {
    std::cout << "Standard Alignment: " << alignof(std::max_align_t) << '\n';

    double *ptr = (double*) malloc(sizeof(double));
    std::cout << "Double Alignment: " << alignof(*ptr) << '\n';

    char *ptr2 = (char*) malloc(1);
    std::cout << "Char Alignment: " << alignof(*ptr2) << '\n';

    void *ptr3;
    std::cout << "Sizeof void*: " << sizeof(ptr3) << '\n';
return 0;
}
```

### Creating Your BUILD File

Now, create a `BUILD` file within your `examples` directory like the following:

```
cc_binary(
    name = "hello_world",
    srcs = ["hello_world.cc"],
    malloc = "@com_google_tcmalloc//tcmalloc",
)
```

NOTE: For more information on how to create Bazel BUILD files, consult the
[Bazel Tutorial](https://docs.bazel.build/versions/master/tutorial/cpp.html).

We declare TCMalloc as our own custom allocation framework using the `malloc`
keyword and set this to the library name (`//tcmalloc`) within our `WORKSPACE`
file (`@com_google_tcmalloc`).

Build our target ("hello_world") and run it:

```
# It's often good practice to build files from the workspace root
$ cd ~/Source/TestProject
Source/TestProject$ bazel build //examples:hello_world --cxxopt='-std=c++17'
INFO: Analysed target //examples:hello_world (12 packages loaded).
INFO: Found 1 target...
Target //examples:hello_world up-to-date:
  bazel-bin/examples/hello_world
INFO: Elapsed time: 0.180s, Critical Path: 0.00s
INFO: Build completed successfully, 1 total action

Source/TestProject$ bazel run //examples:hello_world
INFO: Running command line: bazel-bin/examples/hello_world
Standard Alignment: 16
Double Alignment: 8
Char Alignment: 1
Sizeof void*: 8
Source/TestProject$
```

Note that we passed `--cxxopt='std=c++17'` to build using C++17. Instead of
passing this flag you can add this line to your root `.bazelrc` file:

```
build --cxxopt='-std=c++17'
```

Congratulations! You've created your first binary using TCMalloc.

## What's Next

*   Read our [overview](overview.md), if you haven't already. The overview
    covers memory allocation concepts and best practices for using TCMalloc.
*   Read through the TCMalloc [reference](reference.md) for information on the
    behavior of `malloc()`, `::operator new`, and other allocation/deallocation
    routines in TCMalloc.
*   Consult the TCMalloc C++ `malloc_extension.h` header file, which contains
    information on TCMalloc's supported extensions.
*   Read our [contribution guidelines](../CONTRIBUTING.md), if you intend to
    submit code to our repository.
