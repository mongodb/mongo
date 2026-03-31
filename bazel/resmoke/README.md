# Resmoke Bazel Rules

Rules for running resmoke test suites in Bazel.

## resmoke_suite_test

Runs a resmoke test suite in Bazel.

**EXAMPLE**

```bzl
load("//bazel/resmoke:resmoke.bzl", "resmoke_suite_test")

resmoke_suite_test(
    name = "core",
    config = "//buildscripts/resmokeconfig:suites/core.yml",
    data = [
        "//jstests/libs:all_subpackage_javascript_files",
    ],
    deps = [
        "//src/mongo/db:mongod",
        "//src/mongo/shell:mongo",
    ],
)
```

**ATTRIBUTES**

| Name         | Description                                                                                                                                           | Type                                                                | Mandatory | Default |
| :----------- | :---------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------ | :-------- | :------ |
| name         | A unique name for this test target.                                                                                                                   | <a href="https://bazel.build/concepts/labels#target-names">Name</a> | required  |         |
| config       | The resmoke YAML configuration file for the suite. Must have a `selector.roots` field. Passed directly to resmoke at runtime.                         | <a href="https://bazel.build/concepts/labels">Label</a>             | required  |         |
| srcs         | Override for test source files. If empty (default), automatically derived from the suite YAML's `selector.roots` via the pre-build generator.         | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |
| data         | Additional files required by the tests during runtime. Typically JavaScript library files from jstests/libs.                                          | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |
| deps         | MongoDB binaries and other executables that the tests depend on (e.g., mongod, mongos). These are placed on the PATH when running the test suite.     | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |
| resmoke_args | Additional command-line arguments to pass to the resmoke runner.                                                                                      | List of strings                                                     | optional  | `[]`    |
| shard_count  | The number of parallel shards to split the test suite across. Each shard runs a subset of the tests. See the Test Sharding section below for details. | Integer                                                             | optional  | `None`  |

### Test Sharding

Test sharding allows you to split a large test suite across multiple parallel test executions, significantly reducing total test time. When `shard_count` is specified, Bazel will:

1. Run the test target multiple times in parallel (up to the specified shard count)
2. Each shard receives a unique shard index (0 to N-1)
3. The resmoke runner uses these values to determine which subset of tests to run in each shard
4. Each shard produces its own test output and logs

Note: sharding is an alternative to the resmoke `--jobs` flag, which should not be used with `resmoke_suite_test`.

### Test Logs and Output Directory

Bazel creates a dedicated output directory for each test run under the `bazel-testlogs` symlink in your workspace root.

For a test target `//jstests/suites/query-execution:core`, the outputs are like:

```
bazel-testlogs/jstests/suites/query-execution/core/
├── test.log                           # Primary test output log. Contains the output of resmoke.py
├── test.outputs/
│   ├── report.json                    # Test results in JSON format
│   ├── resource_usage.txt             # Periodically recorded resource usage metrics
│   └── data/                          # The data directory for the resmoke fixture
│       └── job0/
│           ├── mongorunner/
│           └── resmoke/
│               ├── WiredTiger*
│               ├── journal/
│               └── diagnostic.data/
```

### Useful commands

#### Run a single test from a suite:

```
bazel test //jstests/suites/query-execution:core --test_sharding_strategy=disabled --test_arg=jstests/core/js/jssymbol.js
```

#### Run with additional resmoke flags:

Any `--test_arg` in the bazel command will be propagated as a flag to resmoke.py. To modify the resmoke invocation with any of resmoke's flags, add them as `--test_arg`s.

```
# Runs all tests from the core suite with timeseries in their name, twice, with all feature flags enabled.

bazel test //jstests/suites/query-execution:core \
--test_sharding_strategy=disabled \
--test_arg=--repeatTests=2 \
--test_arg=--runAllFeatureFlagTests \
--test_arg=--skipExcludedTests \
`fdfind -t f --full-path ".timeseries\.js$" jstests/core | awk '{print "--test_arg=" $0}'`
```
