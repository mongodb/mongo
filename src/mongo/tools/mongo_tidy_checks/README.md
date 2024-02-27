# MongoDB Custom Clang Tidy module

The MongoDB server includes a custom clang-tidy module which allows server developers to write custom clang-tidy rules for enforcement of best practices, finding bugs, and [large scale refactoring](https://www.youtube.com/watch?v=UfLH7dORav8). Checks for these rules will be run as part of the commit queue and locally with the [clang-tidy vscode plugin](https://wiki.corp.mongodb.com/display/HGTC/Remote+Development+with+Visual+Studio+Code+%28vscode%29+for+MongoDB) or the clang-tidy command line.

Clang-tidy allows precise analysis of the code via the AST it builds internally. Our custom checks are employed through the use of a dynamic library which is loaded via the -load argument to clang-tidy. SDP and other teams plan to implement mongodb customized checks. If you are interested in implementing checks that can help enforce our standards in the codebase (or even just certain components in the codebase), please reach out for more info.

The basics of implementing a check are in the [clang docs](https://releases.llvm.org/12.0.1/tools/clang/tools/extra/docs/clang-tidy/Contributing.html). If you are planning to implement additional checks, it is highly recommended to look at the [existing clang tidy checks](https://github.com/llvm/llvm-project/tree/llvmorg-12.0.1/clang-tools-extra/clang-tidy) as reference starting material. Note that we use v4 toolchain with clang 12.0.1 and the clang tidy check API is subject to change between clang tidy versions. Also note that clang-tidy 12.0.1 release does not include the ability to load a module. This feature was backported into our v4 toolchain clang-tidy.

#### Basic usage of the custom checks

The current directory contains the individual check source files, the main `MongoTidyModule.cpp` source file which registers the checks, and the SConscript responsible for building the check library module. The module will be installed into the DESTDIR, by default `build/install/lib/libmongo_tidy_checks.so`.

Our internal `buildscripts/clang_tidy.py` will automatically check this location and attempt to load the module if it exists. If it is installed to a non-default location you will need to supply the `--check-module` argument with the location to the module.

The check will only be run if you add the name of the check to the `.clang-tidy.in` configuration file. Note you can also customized options for the specific check in this configuration file. Please reference some of the other checks and clang docs on how to add check specific options.

Each check should be contained in its own `cpp` and `h` file, and have one or more unit tests. The h file must be `#include`'d to the `MongoTidyModule.cpp` file where the check class will be registered with a given check name.

#### Adding check unittests

A simple unittest framework is included with the checks so that they will automatically be run in quick, isolated, and minimal fashion. This allows for faster development and ensures the checks continue working.

The `test` directory contains the python unittest script, the test source files, and the SConscript which builds and runs the tests. NOTE: The python unittest script requires arguments to function correctly, you must supply compile_commands.json files matching the correct location and filename to the corresponding tests. For this reason, you should use the scons build as the interface for running the tests as it will create the compile_commands files, and run the unittest script automatically with the correct arguments. To build and test the checks use the scons command `python buildscripts/scons.py --build-profile=compiledb VERBOSE=1 +mongo-tidy-tests`. Note that currently the `--ninja` option does not support running the mongo tidy unittests.

#### Writing your own check checklist

Below is a checklist of all the steps to make sure to perform when writing a new check.

1. Implement the check in the respectively named `.h` and `.cpp` files.
2. Add the check's `#include` to the `MongoTidyModule.cpp`.
3. Register the check class with a check name in the `MongoTidyModule.cpp`.
4. Add the `.cpp` file to the source list in the `SConscript` file.
5. Write a unittest file named `tests/test_{CHECK_NAME}.cpp` which minimally reproduces the issue.
6. Add the test file to the list of test sources in `tests/SConscript`.
7. Add a `def test_{CHECK_NAME}():` function to the `MongoTidyCheck_unittest.py` file which writes the config file, and finds the expected error output in the stdout. Reference the other check funcions for details.
8. Run the scons build with `python buildscripts/scons.py --build-profile=compiledb VERBOSE=1 +mongo-tidy-tests` to run the tests and see the detailed output of each test.

#### Questions and Troubleshooting

If you have any questions please reach out to the `#server-build-help` slack channel.
