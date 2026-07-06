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

| Name              | Description                                                                                                                                           | Type                                                                | Mandatory | Default |
| :---------------- | :---------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------ | :-------- | :------ |
| name              | A unique name for this test target.                                                                                                                   | <a href="https://bazel.build/concepts/labels#target-names">Name</a> | required  |         |
| config            | The resmoke YAML configuration file for the suite. Must have a `selector.roots` field. Passed directly to resmoke at runtime.                         | <a href="https://bazel.build/concepts/labels">Label</a>             | required  |         |
| srcs              | Override for test source files. If empty (default), automatically derived from the suite YAML's `selector.roots` via the pre-build generator.         | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |
| data              | Additional files required by the tests during runtime. Typically JavaScript library files from jstests/libs.                                          | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |
| deps              | MongoDB binaries and other executables that the tests depend on (e.g., mongod, mongos). These are placed on the PATH when running the test suite.     | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |
| resmoke_args      | Additional command-line arguments to pass to the resmoke runner.                                                                                      | List of strings                                                     | optional  | `[]`    |
| shard_count       | The number of parallel shards to split the test suite across. Each shard runs a subset of the tests. See the Test Sharding section below for details. | Integer                                                             | optional  | `None`  |
| multiversion_deps | `multiversion_setup` targets whose downloaded binary directories are passed to resmoke via `--multiversionDir`. See the Multiversion section below.   | <a href="https://bazel.build/concepts/labels">List of labels</a>    | optional  | `[]`    |

### Test Sharding

Test sharding allows you to split a large test suite across multiple parallel test executions,
significantly reducing total test time. When `shard_count` is specified, Bazel will:

1. Run the test target multiple times in parallel (up to the specified shard count)
2. Each shard receives a unique shard index (0 to N-1)
3. The resmoke runner uses these values to determine which subset of tests to run in each shard
4. Each shard produces its own test output and logs

Note: sharding is an alternative to the resmoke `--jobs` flag, which should not be used with
`resmoke_suite_test`.

### Test Logs and Output Directory

Bazel creates a dedicated output directory for each test run under the `bazel-testlogs` symlink in
your workspace root.

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

Any `--test_arg` in the bazel command will be propagated as a flag to resmoke.py. To modify the
resmoke invocation with any of resmoke's flags, add them as `--test_arg`s.

```
# Runs all tests from the core suite with timeseries in their name, twice, with all feature flags enabled.

bazel test //jstests/suites/query-execution:core \
--test_sharding_strategy=disabled \
--test_arg=--repeatTests=2 \
--test_arg=--runAllFeatureFlagTests \
--test_arg=--skipExcludedTests \
`fdfind -t f --full-path ".timeseries\.js$" jstests/core | awk '{print "--test_arg=" $0}'`
```

## multiversion_setup

Downloads old MongoDB binaries for multiversion testing via `db-contrib-tool setup-repro-env`, and
generates companion exclude-tags for `last-lts` and `last-continuous` versions.

A set of pre-defined targets lives in `//bazel/resmoke/multiversion`, such as:

| Target                                         | Version         |
| :--------------------------------------------- | :-------------- |
| `//bazel/resmoke/multiversion:last-lts`        | last-lts        |
| `//bazel/resmoke/multiversion:last-continuous` | last-continuous |
| `//bazel/resmoke/multiversion:7.0`             | 7.0             |
| `//bazel/resmoke/multiversion:8.0.16`          | 8.0.16          |

To test against a specific version, add a new `multiversion_setup` target.

### Using multiversion in a resmoke_suite_test

Pass one or more `multiversion_setup` targets via `multiversion_deps`.

**EXAMPLE**

```bzl
load("//bazel/resmoke:resmoke.bzl", "resmoke_suite_test")

resmoke_suite_test(
    name = "multiversion_sanity_check_last_continuous_new_new_old",
    config = ":multiversion_sanity_check_last_continuous_new_new_old.yml",
    multiversion_deps = [
        "//bazel/resmoke/multiversion:last-continuous",
    ],
    deps = [
        "//src/mongo/db:mongod",
        "//src/mongo/shell:mongo",
    ],
)
```

**ATTRIBUTES**

| Name    | Description                                                                                                                           | Type                                                                | Mandatory |
| :------ | :------------------------------------------------------------------------------------------------------------------------------------ | :------------------------------------------------------------------ | :-------- |
| name    | A unique name for this target. Also used as the name prefix for the `<name>_exclude_tags` target.                                     | <a href="https://bazel.build/concepts/labels#target-names">Name</a> | required  |
| version | MongoDB version string passed to `db-contrib-tool setup-repro-env`. Examples: `"7.0"`, `"8.0.16"`, `"last-lts"`, `"last-continuous"`. | String                                                              | required  |

### Exclude tags

For `last-lts` and `last-continuous` versions, `multiversion_setup` creates a companion
`<name>_exclude_tags` target that runs `resmoke.py generate-multiversion-exclude-tags`. The
resulting YAML file is automatically passed to resmoke via `--tagFile` when the `multiversion_setup`
target appears in `multiversion_deps`, so tests that are incompatible with the old binary version
are skipped without any extra configuration.

### Reproducing a failure with pinned binaries

Each `multiversion_setup` target has a `string_flag` named `<name>-pin`. Passing it on the command
line tells `db-contrib-tool` to download a specific build instead of the current latest:

```
bazel test //buildscripts/resmokeconfig:multiversion_sanity_check_last_continuous_new_new_old \
    --//bazel/resmoke/multiversion:last-continuous-pin=<evg-version-id>
```

The flag accepts any identifier that `db-contrib-tool setup-repro-env` understands:

| Value                | Example                                                         |
| :------------------- | :-------------------------------------------------------------- |
| Evergreen version ID | `6172c9b65623435a4c0bdb1a`                                      |
| Full git commit hash | `d9c83ee0c93970029e41234c77dc20b2c5ca6291`                      |
| Evergreen task ID    | `mongodb_mongo_master_enterprise_rhel_80_..._22_02_16_03_30_27` |

For tests with multiple multiversion deps, pass one flag per version.

**Finding the EVG version ID from a previous run**

Every test run writes a `multiversion-downloads.json` file recording the exact Evergreen version
that was downloaded. The file is preserved per-run in the test outputs:

```
bazel-testlogs/.../test.outputs/multiversion-downloads-last-continuous.json
```

Use the ID from that file with `--//bazel/resmoke/multiversion:last-continuous-pin` to reproduce the
failure with identical binaries.

## jstestfuzz_generate

Generates a directory of randomized `.js` test files by invoking the
[10gen/jstestfuzz](https://github.com/10gen/jstestfuzz) generator, and feeds them into a
`resmoke_suite_test` via `srcs`.

**EXAMPLE**

```bzl
load("//bazel/resmoke:jstestfuzz.bzl", "jstestfuzz_generate")
load("//bazel/resmoke:resmoke.bzl", "resmoke_suite_test")

jstestfuzz_generate(
    name = "jstestfuzz_generated",
    npm_command = "jstestfuzz",
    num_generated_files = 10,
)

resmoke_suite_test(
    name = "jstestfuzz",
    srcs = [":jstestfuzz_generated"],
    config = ":suites/jstestfuzz.yml",
    deps = [
        "//src/mongo/db:mongod",
        "//src/mongo/shell:mongo",
    ],
)
```

**ATTRIBUTES**

| Name                | Description                                                                                                                                         | Type            | Mandatory | Default        |
| :------------------ | :-------------------------------------------------------------------------------------------------------------------------------------------------- | :-------------- | :-------- | :------------- |
| name                | A unique name for this target.                                                                                                                      | Name            | required  |                |
| num_generated_files | How many `.js` test files to emit. Forwarded to jstestfuzz as `--numGeneratedFiles`.                                                                | Integer         | required  |                |
| seed                | Fixed seed for this target. Overrides `--//bazel/resmoke:jstestfuzz_seed` when set. Leave empty to use the flag (random per build by default).      | String          | optional  | `""`           |
| npm_command         | The npm script in jstestfuzz's `package.json` to run (e.g. `jstestfuzz`, `agg-fuzzer`, `query-fuzzer`, `update-fuzzer`, `rollback-fuzzer`).         | String          | optional  | `"jstestfuzz"` |
| use_es_modules      | Pass `--useEsModules` to jstestfuzz. Required for npm commands other than the default `jstestfuzz`.                                                 | Boolean         | optional  | `False`        |
| extra_args          | Additional CLI flags forwarded verbatim to jstestfuzz (e.g. `["--opType", "moveCollection"]`). `--jsTestsDir` defaults to the workspace `jstests/`. | List of strings | optional  | `[]`           |

### Reproducing a failure

Two flags must match for byte-identical reproduction: the seed used by jstestfuzz and the upstream
commit it was generated from.

| Flag                                    | What it pins                                                         |
| :-------------------------------------- | :------------------------------------------------------------------- |
| `--//bazel/resmoke:jstestfuzz_seed=<n>` | The seed passed to jstestfuzz (default: random per build).           |
| `--repo_env=JSTESTFUZZ_COMMIT=<sha>`    | The upstream jstestfuzz commit to clone (default: head of `master`). |

Example:

```
bazel test //buildscripts/resmokeconfig:jstestfuzz \
    --//bazel/resmoke:jstestfuzz_seed=42 \
    --repo_env=JSTESTFUZZ_COMMIT=f21b49c53824f677af60f766eda029366e82513e
```

**Finding the seed and commit from a previous run**

Each test run records both values into `test.outputs/`:

```
bazel-testlogs/.../test.outputs/jstestfuzz_seed.txt
bazel-testlogs/.../test.outputs/jstestfuzz_commit_sha.txt
```
