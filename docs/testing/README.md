# Testing

Most tests for MongoDB are run through resmoke, our test runner and orchestration tool.
The entry point for resmoke can be found at `buildscripts/resmoke.py`

## run

The run subcommand can run suites (list of tests and the MongoDB topology and
configuration to run them against), and explicitly named test files.

A single suite can be specified using the `--suite` flag, and multiple suites
can be specified by providing a comma separated list to the `--suites` flag.

Additional parameters for the run subcommand can be found on the help page,
accessible by running `buildscripts/resmoke.py run --help`

Additional documentation on our suite configuration can be found on the
[Suites configuration file page](../suites.md)

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

We capture telemetry from resmoke using two systems: mongo-tooling-metrics and open telemetry.

Using mongo-tooling-metrics we capture the invocation, results, and timing data from internal developers. This data is used to see what developers are doing. We can study what people are running to make it work better or faster.

Using open telemetry (OTel) we capture more specific information about the internals of resmoke. This data is used for improvements specifically when running in evergreen. This data is captured on every resmoke invocation but only sent to honeycomb when running in evergreen. More info about how we use OTel in resmoke can be found [here](otel_resmoke.md).
