# Multiversion Testing

## Table of contents

- [Multiversion Testing](#multiversion-testing)
  - [Table of contents](#table-of-contents)
  - [Terminology and overview](#terminology-and-overview)
    - [Introduction](#introduction)
    - [Latest vs last-lts vs last-continuous](#latest-vs-last-lts-vs-last-continuous)
    - [Old vs new](#old-vs-new)
    - [Explicit and Implicit multiversion suites](#explicit-and-implicit-multiversion-suites)
    - [Version combinations](#version-combinations)
  - [Working with multiversion tasks in Evergreen](#working-with-multiversion-tasks-in-evergreen)
    - [Exclude tests from multiversion testing](#exclude-tests-from-multiversion-testing)
    - [Multiversion task generation](#multiversion-task-generation)

## Terminology and overview

### Introduction

Some tests test specific upgrade/downgrade behavior expected between different versions of MongoDB.
Several versions of MongoDB are spun up during those test runs.

- Multiversion suites - resmoke suites that are running tests with several versions of MongoDB.

- Multiversion tasks - Evergreen tasks that are running multiversion suites. Multiversion tasks in
  most cases include `multiversion` or `downgrade` in their names.

### Latest vs last-lts vs last-continuous

For some of the versions we are using such generic names as `latest`, `last-lts` and
`last-continuous`.

- `latest` - the current version. In Evergreen, the version that was compiled in the current build.

- `last-lts` - the latest LTS (Long Term Support) Major release version. In Evergreen, the version
  that was downloaded from the last LTS release branch project. It resolves to an entry
  in `longTermSupportReleases` of [releases.yml](../../src/mongo/util/version/releases.yml).

- `last-continuous` - the latest Rapid release version. In Evergreen, the version that was
  downloaded from the Rapid release branch project. It resolves to the entry in
  `featureCompatibilityVersions` of [releases.yml](../../src/mongo/util/version/releases.yml)
  that looks older than the output of `git describe`. Will not be tested against if it is listed in
  `eolVersions` as being end of life.

Note: The latest release.yml file from master is always used, even fetched remotely when on another branch.

### Old vs new

Many multiversion tasks are running tests against `latest`/`last-lts` or `latest`/`last-continuous`
versions. In such context we refer to `last-lts` and `last-continuous` versions as the `old`
version and to `latest` as a `new` version.

A `new` version is compiled in the same way as for non-multiversion tasks. The `old` versions of
compiled binaries are downloaded from the old branch projects with
[`db-contrib-tool`](https://github.com/10gen/db-contrib-tool).
`db-contrib-tool` searches for the latest available compiled binaries on the old branch projects in
Evergreen.

### Explicit and Implicit multiversion suites

Multiversion suites can be explicit and implicit.

- Explicit - JS tests are aware of the binary versions they are running,
  e.g. [multiversion.yml](https://github.com/mongodb/mongo/blob/e91cda950e50aa4c707efbdd0be208481493fc96/buildscripts/resmokeconfig/suites/multiversion.yml).
  The version of binaries is explicitly set in JS tests,
  e.g. [jstests/multiVersion/genericSetFCVUsage/major_version_upgrade.js](https://github.com/mongodb/mongo/blob/397c8da541940b3fbe6257243f97a342fe7e0d3b/jstests/multiVersion/genericSetFCVUsage/major_version_upgrade.js#L33-L44):

```js
const versions = [
  {
    binVersion: "4.4",
    featureCompatibilityVersion: "4.4",
    testCollection: "four_four",
  },
  {
    binVersion: "5.0",
    featureCompatibilityVersion: "5.0",
    testCollection: "five_zero",
  },
  {
    binVersion: "6.0",
    featureCompatibilityVersion: "6.0",
    testCollection: "six_zero",
  },
  {
    binVersion: "last-lts",
    featureCompatibilityVersion: lastLTSFCV,
    testCollection: "last_lts",
  },
  {
    binVersion: "last-continuous",
    featureCompatibilityVersion: lastContinuousFCV,
    testCollection: "last_continuous",
  },
  {
    binVersion: "latest",
    featureCompatibilityVersion: latestFCV,
    testCollection: "latest",
  },
];
```

- Implicit - JS tests know nothing about the binary versions they are running,
  e.g. [retryable_writes_downgrade.yml](https://github.com/mongodb/mongo/blob/e91cda950e50aa4c707efbdd0be208481493fc96/buildscripts/resmokeconfig/suites/retryable_writes_downgrade.yml).
  Most of the implicit multiversion suites are using matrix suites, e.g. `replica_sets_last_lts`:

```bash
$ python buildscripts/resmoke.py suiteconfig --suite=replica_sets_last_lts

executor:
  config:
    shell_options:
      global_vars:
        TestData:
          useRandomBinVersionsWithinReplicaSet: last-lts
      nodb: ''
selector:
  exclude_files:
  - jstests/replsets/initial_sync_rename_collection.js
  - jstests/replsets/initial_sync_drop_collection.js
  - jstests/replsets/apply_prepare_txn_write_conflict_robustness.js
  - jstests/replsets/invalidate_sessions_on_stepdown.js
  - jstests/replsets/initial_sync_fails_unclean_restart.js
  exclude_with_any_tags:
  - multiversion_incompatible
  - backport_required_multiversion
  - replica_sets_multiversion_backport_required_multiversion
  - disabled_for_fcv_6_1_upgrade
  roots:
  - jstests/replsets/*.js
test_kind: js_test
```

In implicit multiversion suites the version of binaries is defined on the resmoke fixture level.

The [example](https://github.com/mongodb/mongo/blob/e91cda950e50aa4c707efbdd0be208481493fc96/buildscripts/resmokeconfig/matrix_suites/overrides/multiversion.yml#L5-L8)
of replica set fixture configuration override:

```yaml
fixture:
  num_nodes: 3
  old_bin_version: last_lts
  mixed_bin_versions: new_new_old
```

The [example](https://github.com/mongodb/mongo/blob/e91cda950e50aa4c707efbdd0be208481493fc96/buildscripts/resmokeconfig/matrix_suites/overrides/multiversion.yml#L53-L57)
of sharded cluster fixture configuration override:

```yaml
fixture:
  num_shards: 2
  num_rs_nodes_per_shard: 2
  old_bin_version: last_lts
  mixed_bin_versions: new_old_old_new
```

The [example](https://github.com/mongodb/mongo/blob/e91cda950e50aa4c707efbdd0be208481493fc96/buildscripts/resmokeconfig/matrix_suites/overrides/multiversion.yml#L139-L145)
of shell fixture configuration override:

```yaml
value:
  executor:
    config:
      shell_options:
        global_vars:
          TestData:
            useRandomBinVersionsWithinReplicaSet: "last-lts"
```

### Version combinations

In implicit multiversion suites the same set of tests may run in similar suites that are using
various mixed version combinations. Those version combinations depend on the type of resmoke
fixture the suite is running with. These are the recommended version combinations to test against based on the suite fixtures:

- Replica set fixture combinations:

  - `last-lts new-new-old` (i.e. suite runs the replica set fixture that spins up the `latest` and
    the `last-lts` versions in a 3-node replica set where the 1st node is the `latest`, 2nd - `latest`,
    3rd - `last-lts`, etc.)
  - `last-lts new-old-new`
  - `last-lts old-new-new`
  - `last-continuous new-new-old`
  - `last-continuous new-old-new`
  - `last-continuous old-new-new`
  - Ex: [change_streams](https://github.com/mongodb/mongo/blob/88d59bfe9d5ee2c9938ae251f7a77a8bf1250a6b/buildscripts/resmokeconfig/suites/change_streams.yml) uses a [`ReplicaSetFixture`](https://github.com/mongodb/mongo/blob/88d59bfe9d5ee2c9938ae251f7a77a8bf1250a6b/buildscripts/resmokeconfig/suites/change_streams.yml#L50) so the corresponding multiversion suites are
    - [`change_streams_last_continuous_new_new_old`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/change_streams_last_continuous_new_new_old.yml)
    - [`change_streams_last_continuous_new_old_new`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/change_streams_last_continuous_new_old_new.yml)
    - [`change_streams_last_continuous_old_new_new`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/change_streams_last_continuous_old_new_new.yml)
    - [`change_streams_last_lts_new_new_old`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/change_streams_last_lts_new_new_old.yml)
    - [`change_streams_last_lts_new_old_new`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/change_streams_last_lts_new_old_new.yml)
    - [`change_streams_last_lts_old_new_new`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/change_streams_last_lts_old_new_new.yml)

- Sharded cluster fixture combinations:

  - `last-lts new-old-old-new` (i.e. suite runs the sharded cluster fixture that spins up the
    `latest` and the `last-lts` versions in a sharded cluster that consists of 2 shards with 2-node
    replica sets per shard where the 1st node of the 1st shard is the `latest`, 2nd node of 1st
    shard - `last-lts`, 1st node of 2nd shard - `last-lts`, 2nd node of 2nd shard - `latest`, etc.)
  - `last-continuous new-old-old-new`
  - Ex: [change_streams_downgrade](https://github.com/mongodb/mongo/blob/a96b83b2fa7010a5823fefac2469b4a06a697cf1/buildscripts/resmokeconfig/suites/change_streams_downgrade.yml) uses a [`ShardedClusterFixture`](https://github.com/mongodb/mongo/blob/a96b83b2fa7010a5823fefac2469b4a06a697cf1/buildscripts/resmokeconfig/suites/change_streams_downgrade.yml#L408) so the corresponding multiversion suites are
    - [`change_streams_downgrade_last_continuous_new_old_old_new`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/change_streams_downgrade_last_continuous_new_old_old_new.yml)
    - [`change_streams_downgrade_last_lts_new_old_old_new`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/change_streams_downgrade_last_lts_new_old_old_new.yml)

- Shell fixture combinations:
  - `last-lts` (i.e. suite runs the shell fixture that spins up `last-lts` as the `old` versions,
    etc.)
  - `last-continuous`
  - Ex: [initial_sync_fuzzer](https://github.com/mongodb/mongo/blob/908625ffdec050a71aa2ce47c35788739f629c60/buildscripts/resmokeconfig/suites/initial_sync_fuzzer.yml) uses a Shell Fixture, so the corresponding multiversion suites are
    - [`initial_sync_fuzzer_last_lts`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/initial_sync_fuzzer_last_lts.yml)
    - [`initial_sync_fuzzer_last_continuous`](https://github.com/mongodb/mongo/blob/612814f4ce56282c47d501817ba28337c26d7aba/buildscripts/resmokeconfig/matrix_suites/mappings/initial_sync_fuzzer_last_continuous.yml)

If `last-lts` and `last-continuous` versions happen to be the same, or last-continuous is EOL, we skip `last-continuous`
and run multiversion suites with only `last-lts` combinations in Evergreen.

## Working with multiversion tasks in Evergreen

### Multiversion task generation

Please refer to mongo-task-generator [documentation](https://github.com/mongodb/mongo-task-generator/blob/master/docs/generating_tasks.md#multiversion-testing)
for generating multiversion tasks in Evergreen.

### Exclude tests from multiversion testing

Sometimes tests are not designed to run in multiversion suites. To avoid implicit multiversion
suites to pick up the test `multiversion_incompatible` tag can be added to the test, e.g.
[jstests/concurrency/fsm_workloads/drop_database_sharded_setFCV.js](https://github.com/mongodb/mongo/blob/fcdfe29cee066278b94ea2749456fc433cc398c6/jstests/concurrency/fsm_workloads/drop_database_sharded_setFCV.js#L9).

```js
// @tags: [multiversion_incompatible]
```

There is additional `requires_fcv_XX` set of tags that can be used to disable tests from running in
multiversion where `XX` is the version number, e.g. `requires_fcv_70` stands for version `7.0` in
[jstests/auth/user_roles_empty.js](https://github.com/mongodb/mongo/blob/4170a47e6af669e411c9809e676f46171d0a633f/jstests/auth/user_roles_empty.js#L3).

```js
// @tags: [requires_fcv_61]
```

Tests with `requires_fcv_XX` tags are excluded from multiversion tasks that may run the versions
below the specified FCV version, e.g. when the `latest` version is `6.2`, `last-continuous` is
`6.1` and `last-lts` is `6.0`, tests tagged with `requires_fcv_61` will NOT run in multiversion
tasks that run `latest` with `last-lts`, but will run in multiversion tasks that run `lastest` with
`last-continuous`.

In addition to disabling multiversion tests based on FCV, there is no need to run in-development `featureFlagXYZ` tests
(featureFlags that have `default: false`) because these tests will most likely fail on older versions that
have not implemented this feature. For multiversion tasks, we pass the `--runNoFeatureFlagTests` flag to avoid these
failures on `all feature flag` variants.

For more info on FCV, take a look at [FCV_AND_FEATURE_FLAG_README.md](https://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/FCV_AND_FEATURE_FLAG_README.md).

Another common case could be that the changes on master branch are breaking multiversion tests,
but with those changes backported to the older branches the multiversion tests should work.
In order to temporarily disable the test from running in multiversion it can be added to the
[etc/backports_required_for_multiversion_tests.yml](https://github.com/mongodb/mongo/blob/fcdfe29cee066278b94ea2749456fc433cc398c6/etc/backports_required_for_multiversion_tests.yml#L1-L19).
Please follow the instructions described in the file.
