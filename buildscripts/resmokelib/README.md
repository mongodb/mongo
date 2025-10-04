# Resmoke

Resmoke is MongoDB's integration test runner.

The JS Tests it can run live in the `jstests/` directory - reference its [README](../../jstests/README.md) to learn about their content.

## Build

Though the source is built with bazel, resmoke is not yet integrated. This means that the source has to be built prior to using resmoke, eg:

```
bazel build install-dist-test
```

## Actions

<!-- the following is a copy-paste from `resmoke --help` -->

```
    run                 Runs the specified tests.
    list-suites         Lists the names of the suites available to execute.
    generate-matrix-suites
                        Generate matrix suite config files from the mapping files.
    find-suites         Lists the names of the suites that will execute the specified tests.
    list-tags           Lists the tags and their documentation available in the suites.
    generate-multiversion-exclude-tags
                        Create a tag file associating multiversion tests to tags for exclusion. Compares the BACKPORTS_REQUIRED_FILE on the current branch with the same file on the last-lts and/or last-continuous branch to determine which tests should be
                        denylisted.
    core-analyzer       Analyzes the core dumps from the specified input files.
    hang-analyzer       Hang Analyzer module. A prototype hang analyzer for Evergreen integration to help investigate test timeouts 1. Script supports taking dumps, and/or dumping a summary of useful information about a process 2. Script will iterate
                        through a list of interesting processes, and run the tools from step 1. The list of processes can be provided as an option. 3. Java processes will be dumped using jstack, if available. Supports Linux, MacOS X, and Windows.
    powercycle          Powercycle test. Tests robustness of mongod to survive multiple powercycle events. Client & server side powercycle test script. This script is used in conjunction with certain Evergreen hosts created with the `evergreen host
                        create` command.
    generate-fcv-constants
                        ==SUPPRESS==
    test-discovery      Discover what tests are run by a suite.
    suiteconfig         Display configuration of a test suite.
    multiversion-config
                        Display configuration for multiversion testing
    generate-fuzz-config
                        Generate a mongod.conf and mongos.conf using config fuzzer.
```

Note: `bisect`, `setup-multiversion`, and `symbolize` commands have been moved to [`db-contrib-tool`](https://github.com/10gen/db-contrib-tool#readme).

## Suites

Many of the above commands use the concept of a "suite". Loosely, suites group which tests run, and how.

Read more about suites [here](../../buildscripts/resmokeconfig/suites/README.md).

## run

The `run` command is the most used feature of resmoke.

The most typical approach is to run a particular JS test file given a suite, eg:

```
buildscripts/resmoke.py run --suites=no_passthrough jstests/noPassthrough/shell/js/string.js
```

That executes the content of that file, using the suite configuration as a fixture setup. The suite "no_passthrough" is associated with the file [buildscripts/resmokeconfig/suites/no_passthrough.yml](../../buildscripts/resmokeconfig/suites/no_passthrough.yml).

Run has **100+ flags**! Use `resmoke run --help` to inspect them. To avoid risk of multiple sources of truth that can drift and become stale, **we do not attempt to document them all here** - they should each be self-descriptive and documented within the CLI help.

Below are very high-level descriptions for high-usage flags.

### Suites (`--suites`)

The run subcommand can run suites (list of tests and the MongoDB topology and
configuration to run them against), and explicitly named test files.

A single suite can be specified using the `--suite` flag, and multiple suites
can be specified by providing a comma separated list to the `--suites` flag.

Additional documentation on our suite configuration can be found in
[buildscripts/resmokeconfig/suites/README.md](../../buildscripts/resmokeconfig/suites/README.md).

### Testable Installations (`--installDir`)

resmoke can run tests against any testable installation of MongoDB (such
as ASAN, Debug, Release). When possible, resmoke will automatically locate and
run with a locally built copy of MongoDB Server, so long as that build was
installed to a subdirectory of the root of the git repository, and there is
exactly one build. In other situations, the `--installDir` flag, passed to run
subcommand, can be used to indicate the location of the mongod/mongos binaries.

As an alternative, you may instead prefer to use the resmoke.py wrapper script
located in the same directory as the mongod binary, which will automatically
set `installDir` for you.

Note that this wrapper is unavailable in packaged installations of MongoDB
Server, such as those provided by Homebrew, and other package managers. If you
would like to run tests against a packaged installation, you must explicitly
pass `--installDir` to resmoke.py

### Resmoke test telemetry

We capture telemetry from resmoke using open telemetry.

Using open telemetry (OTel) we capture more specific information about the internals of resmoke. This data is used for improvements specifically when running in evergreen. This data is captured on every resmoke invocation but only sent to honeycomb when running in evergreen. More info about how we use OTel in resmoke can be found [here](otel_resmoke.md).
