# MongoDB Custom Clang Tidy module

The MongoDB server includes a custom clang-tidy module which allows server developers to write custom clang-tidy rules for enforcement of best practices, finding bugs, and [large scale refactoring](https://www.youtube.com/watch?v=UfLH7dORav8). Checks for these rules will be run as part of the commit queue and locally with the [clang-tidy vscode plugin](https://wiki.corp.mongodb.com/display/HGTC/Remote+Development+with+Visual+Studio+Code+%28vscode%29+for+MongoDB) or the clang-tidy command line.

Clang-tidy allows precise analysis of the code via the AST it builds internally. Our custom checks are employed through the use of a dynamic library which is loaded via the -load argument to clang-tidy. SDP and other teams plan to implement mongodb customized checks. If you are interested in implementing checks that can help enforce our standards in the codebase (or even just certain components in the codebase), please reach out for more info.

The basics of implementing a check are in the [clang docs](https://releases.llvm.org/12.0.1/tools/clang/tools/extra/docs/clang-tidy/Contributing.html). If you are planning to implement additional checks, it is highly recommended to look at the [existing clang tidy checks](https://github.com/llvm/llvm-project/tree/llvmorg-12.0.1/clang-tools-extra/clang-tidy) as reference starting material. Note that we use v4 toolchain with clang 12.0.1 and the clang tidy check API is subject to change between clang tidy versions. Also note that clang-tidy 12.0.1 release does not include the ability to load a module. This feature was backported into our v4 toolchain clang-tidy.

#### Basic usage of the custom checks

The current directory contains the individual check source files, the main `MongoTidyModule.cpp` source file which registers the checks, and the BUILD.bazel file responsible for building the check library module. To build the custom checks use this command:

The bazel clang-tidy config will automatically build and use the custom checks module. To run clang tidy with the checks module use:

    bazel build --config=clang-tidy //src/mongo/...

#### Adding check unittests

A simple unittest framework is included with the checks so that they will automatically be run in quick, isolated, and minimal fashion. This allows for faster development and ensures the checks continue working.

The `test` directory contains the python unittest script, the test source files, and the BUILD.bazel which builds and runs the tests. To run all the custom tidy rules unittests use:

    bazel run mongo-tidy-test

This will run all the tests serially. Eventually we can improve this and they can be run in parallel but currently they are serial. To run a specific test you can use python's unittest args, for example:

bazel run mongo-tidy-test -- -k test_MongoInvariantStatusIsOKCheck

#### Writing your own check checklist

Below is a checklist of all the steps to make sure to perform when writing a new check.

1. Implement the check in the respectively named `.h` and `.cpp` files.
2. Add the check's `#include` to the `MongoTidyModule.cpp`.
3. Register the check class with a check name in the `MongoTidyModule.cpp`.
4. Add the `.cpp` file to the source list in the `BUILD.bazel` file.
5. Write a unittest file named `tests/test_{CHECK_NAME}.cpp` and a config file `tests/test_{CHECK_NAME}.tidy_config` which minimally reproduces the issue.
6. Add the test to the list of tests in `tests/BUILD.bazel`.
7. Add a `def test_{CHECK_NAME}():` function to the `MongoTidyCheck_unittest.py` file which finds the expected error output in the stdout. Reference the other check functions for details.
8. Run the bazel build with `bazel run mongo-tidy-test -- -k test_{CHECK_NAME}` to run the newly created test.

#### Questions and Troubleshooting

If you have any questions please reach out to the `#ask-devprod-build` slack channel.
