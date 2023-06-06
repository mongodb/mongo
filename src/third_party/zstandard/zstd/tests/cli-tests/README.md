# CLI tests

The CLI tests are focused on testing the zstd CLI.
They are intended to be simple tests that the CLI and arguments work as advertised.
They are not intended to test the library, only the code in `programs/`.
The library will get incidental coverage, but if you find yourself trying to trigger a specific condition in the library, this is the wrong tool.

## Test runner usage

The test runner `run.py` will run tests against the in-tree build of `zstd` and `datagen` by default. Which means that `zstd` and `datagen` must be built.

The `zstd` binary used can be passed with `--zstd /path/to/zstd`.
Additionally, to run `zstd` through a tool like `valgrind` or `qemu`, set the `--exec-prefix 'valgrind -q'` flag.

Similarly, the `--datagen`, and `--zstdgrep` flags can be set to specify
the paths to their respective binaries. However, these tools do not use
the `EXEC_PREFIX`.

Each test executes in its own scratch directory under `scratch/test/name`. E.g. `scratch/basic/help.sh/`. Normally these directories are removed after the test executes. However, the `--preserve` flag will preserve these directories after execution, and save the tests exit code, stdout, and stderr in the scratch directory to `exit`, `stderr`, and `stdout` respectively. This can be useful for debugging/editing a test and updating the expected output.

### Running all the tests

By default the test runner `run.py` will run all the tests, and report the results.

Examples:

```
./run.py
./run.py --preserve
./run.py --zstd ../../build/programs/zstd --datagen ../../build/tests/datagen
```

### Running specific tests

A set of test names can be passed to the test runner `run.py` to only execute those tests.
This can be useful for writing or debugging a test, especially with `--preserve`.

The test name can either be the path to the test file, or the test name, which is the path relative to the test directory.

Examples:

```
./run.py basic/help.sh
./run.py --preserve basic/help.sh basic/version.sh
./run.py --preserve --verbose basic/help.sh
```

### Updating exact output

If a test is failing because a `.stderr.exact` or `.stdout.exact` no longer matches, you can re-run the tests with `--set-exact-output` and the correct output will be written.

Example:
```
./run.py --set-exact-output
./run.py basic/help.sh --set-exact-output
```

## Writing a test

Test cases are arbitrary executables, and can be written in any language, but are generally shell scripts.
After the script executes, the exit code, stderr, and stdout are compared against the expectations.

Each test is run in a clean directory that the test can use for intermediate files. This directory will be cleaned up at the end of the test, unless `--preserve` is passed to the test runner. Additionally, the `setup` script can prepare the directory before the test runs.

### Calling zstd, utilities, and environment variables

The `$PATH` for tests is prepended with the `bin/` sub-directory, which contains helper scripts for ease of testing.
The `zstd` binary will call the zstd binary specified by `run.py` with the correct `$EXEC_PREFIX`.
Similarly, `datagen`, `unzstd`, `zstdgrep`, `zstdcat`, etc, are provided.

Helper utilities like `cmp_size`, `println`, and `die` are provided here too. See their scripts for details.

Common shell script libraries are provided under `common/`, with helper variables and functions. They can be sourced with `source "$COMMON/library.sh`.

Lastly, environment variables are provided for testing, which can be listed when calling `run.py` with `--verbose`.
They are generally used by the helper scripts in `bin/` to coordinate everything.

### Basic test case

When executing your `$TEST` executable, by default the exit code is expected to be `0`. However, you can provide an alternate expected exit code in a `$TEST.exit` file.

When executing your `$TEST` executable, by default the expected stderr and stdout are empty. However, you can override the default by providing one of three files:

* `$TEST.{stdout,stderr}.exact`
* `$TEST.{stdout,stderr}.glob`
* `$TEST.{stdout,stderr}.ignore`

If you provide a `.exact` file, the output is expected to exactly match, byte-for-byte.

If you provide a `.glob` file, the output is expected to match the expected file, where each line is interpreted as a glob syntax. Additionally, a line containing only `...` matches all lines until the next expected line matches.

If you provide a `.ignore` file, the output is ignored.

#### Passing examples

All these examples pass.

Exit 1, and change the expectation to be 1.

```
exit-1.sh
---
#!/bin/sh
exit 1
---

exit-1.sh.exit
---
1
---
```

Check the stdout output exactly matches.

```
echo.sh
---
#!/bin/sh
echo "hello world"
---

echo.sh.stdout.exact
---
hello world
---
```

Check the stderr output using a glob.

```
random.sh
---
#!/bin/sh
head -c 10 < /dev/urandom | xxd >&2
---

random.sh.stderr.glob
---
00000000: * * * * *                 *
```

Multiple lines can be matched with ...

```
random-num-lines.sh
---
#!/bin/sh
echo hello
seq 0 $RANDOM
echo world
---

random-num-lines.sh.stdout.glob
---
hello
0
...
world
---
```

#### Failing examples

Exit code is expected to be 0, but is 1.

```
exit-1.sh
---
#!/bin/sh
exit 1
---
```

Stdout is expected to be empty, but isn't.

```
echo.sh
---
#!/bin/sh
echo hello world
```

Stderr is expected to be hello but is world.

```
hello.sh
---
#!/bin/sh
echo world >&2
---

hello.sh.stderr.exact
---
hello
---
```

### Setup & teardown scripts

Finally, test writing can be eased with setup and teardown scripts.
Each directory in the test directory is a test-suite consisting of all tests within that directory (but not sub-directories).
This test suite can come with 4 scripts to help test writing:

* `setup_once`
* `teardown_once`
* `setup`
* `teardown`

The `setup_once` and `teardown_once` are run once before and after all the tests in the suite respectively.
They operate in the scratch directory for the test suite, which is the parent directory of each scratch directory for each test case.
They can do work that is shared between tests to improve test efficiency.
For example, the `dictionaries/setup_once` script builds several dictionaries, for use in the `dictionaries` tests.

The `setup` and `teardown` scripts run before and after each test case respectively, in the test case's scratch directory.
These scripts can do work that is shared between test cases to make tests more succinct.
For example, the `dictionaries/setup` script copies the dictionaries built by the `dictionaries/setup_once` script into the test's scratch directory, to make them easier to use, and make sure they aren't accidentally modified.

#### Examples

```
basic/setup
---
#!/bin/sh
# Create some files for testing with
datagen > file
datagen > file0
datagen > file1
---

basic/test.sh
---
#!/bin/sh
zstd file file0 file1
---

dictionaries/setup_once
---
#!/bin/sh
set -e

mkdir files/ dicts/
for i in $(seq 10); do
	datagen -g1000 > files/$i
done

zstd --train -r files/ -o dicts/0
---

dictionaries/setup
---
#!/bin/sh

# Runs in the test case's scratch directory.
# The test suite's scratch directory that
# `setup_once` operates in is the parent directory.
cp -r ../files ../dicts .
---
```
