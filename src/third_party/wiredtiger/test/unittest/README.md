# WiredTiger unit tests

## Building/running

These tests are built as part of the default build via CMake + Ninja. To run
them, go to your build directory and execute tests/unittest/unittests.

To run tests for a specific tag (a.k.a. subsystem), put the tag in square
brackets, e.g. `[extent_list]`. You can specify multiple tags using commas.

You can also use the `--list-tags` option if you're not sure, or even the
`--help` flag if you're curious and/or lost. There's also further
command-line help at
https://github.com/catchorg/Catch2/blob/devel/docs/command-line.md.

## Adding tests

If you want add new tests to an existing subsytem, simply edit the relevant
.cpp file. If you want to test a new subsystem, or a subsystem with no
existing tests, create a new .cpp file and add it to the `SOURCES` list in
`create_test_executable()` (in `CMakeLists.txt`).
