# Resmoke Bazel Rules

Rules for running resmoke test suites in Bazel.

## resmoke_suite_test

Runs a resmoke test suite in Bazel.

This rule generates a resmoke configuration file based on the provided test sources and base config,
then executes the tests using the resmoke.py test runner.

**EXAMPLE**

```bzl
load("//bazel/resmoke:resmoke.bzl", "resmoke_suite_test")

resmoke_suite_test(
    name = "jscore",
    config = "//buildscripts/resmokeconfig/suites:core.yml",
    srcs = ["//jstests/core:all_subpackage_javascript_files"],
    exclude_with_any_tags = ["requires_sharding"],
    deps = [
        "//src/mongo/db:mongod",
        "//src/mongo/shell:mongo",
    ],
)
```

**ATTRIBUTES**

| Name                  | Description                                                                                                                                                                        | Type                                                                | Mandatory | Default |
| :-------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------ | :-------- | :------ |
| name                  | A unique name for this test target.                                                                                                                                                | <a href="https://bazel.build/concepts/labels#target-names">Name</a> | required  |         |
| config                | The base resmoke YAML configuration file for the suite. This provides the default configuration that will be augmented with the selected tests.                                    | <a href="https://bazel.build/concepts/labels">Label</a>             | required  |         |
| srcs                  | Test files to include in the suite. These files are written as the 'roots' of the test selector in the generated configuration. Typically JavaScript test files for jstest suites. | <a href="https://bazel.build/concepts/labels">List of labels</a>    | required  |         |
| data                  | Additional files required by the tests during runtime. Typically JavaScript files from jstests/libs.                                                                               | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |
| deps                  | MongoDB binaries and other executables that the tests depend on (e.g., mongod, mongos). These are placed on the PATH when running the test suite.                                  | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |
| exclude_files         | Test files to explicitly exclude from the suite, even if they appear in srcs.                                                                                                      | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |
| exclude_with_any_tags | List of test tags. Tests with any of these tags will be excluded from the suite.                                                                                                   | List of strings                                                     | optional  | `[]`    |
| include_with_any_tags | List of test tags. Only tests with at least one of these tags will be included in the suite.                                                                                       | List of strings                                                     | optional  | `[]`    |
| resmoke_args          | Additional command-line arguments to pass to the resmoke runner.                                                                                                                   | List of strings                                                     | optional  | `[]`    |
| shard_count           | The number of parallel shards to split the test suite across. Each shard runs a subset of the tests. See the Test Sharding section below for details.                              | Integer                                                             | optional  | `None`  |

### Test Sharding

Test sharding allows you to split a large test suite across multiple parallel test executions, significantly reducing total test time. When `shard_count` is specified, Bazel will:

1. Run the test target multiple times in parallel (up to the specified shard count)
2. Each shard receives a unique shard index (0 to N-1)
3. The resmoke runner uses these values to determine which subset of tests to run in each shard
4. Each shard produces its own test output and logs

Note: sharding is an alternative to the resmoke `--jobs` flag, which should not be used with `resmoke_suite_test`.

### Test Logs and Output Directory

Bazel creates a dedicated output directory for each test run under the `bazel-testlogs` symlink in your workspace root.

For a test target `//buildscripts/resmokeconfig:core`, the outputs are like:

```
bazel-testlogs/buildscripts/resmokeconfig/core/
├── test.log                           # Primary test output log. Contains the output of resmoke.py
├── test.outputs/
│   ├── report.json                    # Test results in JSON format
│   └── data/                          # The data directory for the resmoke fixture
│       └── job0/
│           ├── mongorunner/
│           └── resmoke/
│               ├── WiredTiger*
│               ├── journal/
│               └── diagnostic.data/
```
