# IWYU Analysis tool

This tool will run
[include-what-you-use](https://github.com/include-what-you-use/include-what-you-use)
(IWYU) analysis across the codebase via `compile_commands.json`.

The `iwyu_config.yml` file consists of the current options and automatic
pragma marking. You can exclude files from the analysis here.

The tool has two main modes of operation, `fix` and `check` modes. `fix`
mode will attempt to make changes to the source files based off IWYU's
suggestions. The check mode will simply check if there are any suggestion
at all.

`fix` mode will take a long time to run, as the tool needs to rerun any
source in which a underlying header was changed to ensure things are not
broken, and so therefore ends up recompile the codebase several times over.

For more information please refer the the script `--help` option.

# Example usage:

First you must generate the `compile_commands.json` file via this command:

```
python3 buildscripts/scons.py --build-profile=compiledb compiledb
```

Next you can run the analysis:

```
python3 buildscripts/iwyu/run_iwyu_analysis.py
```

The default mode is fix mode, and it will start making changes to the code
if any changes are found.

# Debugging failures

Occasionally IWYU tool will run into problems where it is unable to suggest
valid changes and the changes will cause things to break (not compile). When
it his a failure it will copy the source and all the header's that were used
at the time of the compilation into a directory where the same command can be
run to reproduce the error.

You can examine the suggested changes in the source and headers and compare
them to the working source tree. Then you can make corrective changes to allow
IWYU to get past the failure.

IWYU is not perfect and it make several mistakes that a human can understand
and fix appropriately.

# Running the tests

This tool includes its own end to end testing. The test directory includes
sub directories which contain source and iwyu configs to run the tool against.
The tests will then compare the results to built in expected results and fail
if the the tests are not producing the expected results.

To run the tests use the command:

```
cd buildscripts/iwyu/test
python3 run_tests.py
```
