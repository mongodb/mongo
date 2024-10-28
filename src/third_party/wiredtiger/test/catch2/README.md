# WiredTiger Catch2 framework tests
Catch2 is a unit testing framework that allows testing to be performed underneath the WiredTiger
API layer. Further more information about the framework can be found here:
https://github.com/catchorg/Catch2

## Building/running

To build these tests, run CMake with `-DHAVE_UNITTEST=1`, then build as usual with Ninja.  To run
them, go to your build directory and execute
`./test/catch2/catch2-unittests`.

To run tests for a specific tag (a.k.a. subsystem), put the tag in square brackets, e.g. 
`[extent_list]`. You can specify multiple tags using commas.

You can also use the `--list-tags` option if you're not sure, or even the `--help` flag if you're
curious and/or lost. There's also further command-line help at
https://github.com/catchorg/Catch2/blob/devel/docs/command-line.md.

## Contributor's Guide
There are two categories of tests that exist in the catch2 framework. One category focuses on 
performing internal tests towards a WiredTiger module and the other category contains any unit test
that do not fit to a module.

### Modular tests
Module specific tests are found under the directory matching the modules name. That directory
contains unit tests under unit/ and api contract tests under api/.

### Miscellaneous tests
Any unit tests that do not belong to a Wirediger module will be contained in the misc_tests
directory.

### Adding a new tes
If you want to add new tests to an existing subsytem, simply edit the relevant .cpp file. If you
want to test a new subsystem, or a subsystem with no existing tests, create a new .cpp file and add
it to the `SOURCES` list in `create_test_executable()` (in `CMakeLists.txt`).

A script can be used that automates all the steps of adding a test under create_test.sh
`Usage: ./create_test.sh [-m module] my_test`
