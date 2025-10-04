# Parallel Code Coverage Measurement

## Introduction

[Code coverage](https://en.wikipedia.org/wiki/Code_coverage) measurement is a useful tool to help understand the
effectiveness of testing and identify any gaps in that testing.

WiredTiger uses [gcov](https://en.wikipedia.org/wiki/Gcov) to measure code coverage and
[gcovr](https://gcovr.com/en/stable/) to generate the subsequent reports.

If it is possible to measure code coverage quickly (ie in no more than approx 15 to 20 min) then that measurement task
can be included in all PR testing. This has the benefit that developers and PR reviewers have information on the degree 
to which the set of tests that have been executed covers the code that has been changed in the PR.

Code coverage measurement requires building WiredTiger and its test code for code coverage, which both generates the
compile-time code coverage files (`.gcno`) and turns off optimisation to ensure that the relationship between each line
of the source code and the binary is maintained.

Once the code has been built, a set of tests is executed which will generate run-time code coverage statistics which
are saved in files (`.gcda`) for later analysis by [gcovr](https://gcovr.com/en/stable/).


## Test selection

The current test selection is based on the results of the Code Coverage Competition to achieve approx 50%
in about 30 minutes using serial testing. Subsequently, a small number of longer running tests which did not
add significantly increase code coverage have been removed. 

Test selection details:
* RTS testing is slow, so it was important to skip slow tests (RTS tests 10,12,14,20,26,35,37,38,39).

The code coverage test set does not currently reach a high enough coverage, but is a good starting point.


## The challenges with parallel code coverage measurement

Running test in parallel speeds up execution, however this raises a number of challenges, including:
* gcov saves its run-time data in `.gcda` files, but when running tests in parallel there is no synchronisation of 
  writing the data from each parallel test and so it is possible that coverage data will be lost.
* WiredTiger tests typically generate databases in subdirectories, and so running multiple tests in the same directory
  in parallel requires each test to store its database(s) in its own subdirectory. 

While the subdirectory challenge is relatively simple to solve, the run-time data file issue is more complex.

gcov provides [two macros](https://gcc.gnu.org/onlinedocs/gcc/Cross-profiling.html) to configure where the `.gcda` 
files are saved: `GCOV_PREFIX` and `GCOV_PREFIX_STRIP`. This means that it is possible to push results (`.gcda` files)
from multiple tests into different target directories, but the `.gcno` then need to be copied into each of those target
directories.

An alternative is to set up multiple build directories, and then run each parallel test in its own directory tree.
Note that [CMake explicitly does NOT support copying build trees](https://gitlab.kitware.com/cmake/community/-/wikis/FAQ#why-does-cmake-use-full-paths-or-can-i-copy-my-build-tree).
Attempting to do so results in some tests failing due to paths being incorrect and so the simplest approach is to 
configure CMake and build WiredTiger in each build directory.


## Current Solution

The current solution uses a 16 CPU core (ARM) instance to be able to run many tests in parallel.

There is a tradeoff for the number of build directories. More build directories (up to a max of the number of CPU
cores) can run more tests in parallel and reduce the total execution time. On the other hand, more build directories
require more compilation work which reduces the ability to use spare cores to accelerate the build using `-j`.

At the time of writing, a good tradeoff is to use 8 build directories (compiling with `-j 2`) leading to a build time
of approx 6.5 minutes and a test time of about 2.5 minutes, with other operations such as report generation taking the
total time up to a total of about 13 minutes. This is fast enough to be used in PR testing

The configuration of the setup operations and the test tasks is defined in
[code_coverage_config.json](code_coverage_config.json). The same set of setup operations are executed in each build 
directory in parallel, and the test tasks are put into buckets (one for each build tree) and then each bucket
is executed in parallel.

`gcovr` searches a directory (in this case the `wiredtiger` directory) and all subdirectories (ie including all the
`build_` directories) for code coverage data. This means that it is not necessary to tell `gcovr` how many build 
directories there are.


## Potential areas for improvement

Ideally, it would not be necessary to build WiredTiger multiple times so it would be good to have a solution that
either supports copying the build tree or sending the `.gcda` and `.gcno` into different directories.

The current bucketing process does not factor in the unevenness of test duration and so it is possible that some
test buckets will complete before others. A single queue would improve this, but the bucket approach was very easy
to implement as an interim approach using
[concurrent.futures.ProcessPoolExecutor](https://docs.python.org/3/library/concurrent.futures.html) and 
[subprocess.run](https://docs.python.org/3/library/subprocess.html).