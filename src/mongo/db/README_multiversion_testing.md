# Overview of Multiversion and Upgrade/Downgrade Testing

> Part of the [FCV and Feature Flag documentation](README.md#fcv-and-feature-flags). See also:
> [README_feature_flags.md](README_feature_flags.md#feature-flag-test-tagging).

There are a variety of tests and passthroughs that test multiversion and/or upgrade downgrade
scenarios. These include but are not limited to:

### Targeted multiversion tests

- These tests are in `jstests/multiVersion/targetedLastLTSFeatures` and
  `jstests/multiVersion/targetedLastContinuousFeatures`
- These tests are specific to the current development cycle. These can/will fail after branching and
  are subject to removal during branching. These tests rely on a specific last-lts version. After
  the next major release, last-lts is a different version than expected, so these are subject to
  failure. Tests in this directory will be removed after the next major release.
- If you want to test upgrade/downgrade behavior for a specific FCV-gated feature flag, your test
  should most likely go in this folder.
- See
  [this README](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/jstests/multiVersion/README.md)
  for more information.

### Generic multiVersion tests

- These tests are in (jstests/multiVersion/genericSetFCVUsage,
  jstests/multiVersion/genericBinVersion, jstests/multiVersion/genericChangeStreams)
- These tests test the general functionality of upgrades/downgrades regardless of version. These
  will persist indefinitely, as they should always pass regardless of MongoDB version. See
  [this README](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/jstests/multiVersion/README.md)
  for more information.

### \*\_uninitialized_fcv_jscore_passthrough

These passthroughs (e.g. replica_sets_uninitialized_fcv_jscore_passthrough) run jsCore tests while
the replica set or sharded cluster has a node that has an uninitialized FCV (while it is in initial
sync) and
[forwards commands to the initial sync node as well](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/jstests/libs/override_methods/send_command_to_initial_sync_node_lib.js#L)
The purpose of the passthroughs is to make sure commands do not hit an invariant as a result of
checking FCV/feature flags incorrectly while the node has not initialized its FCV yet.

### fcv_upgrade\_\_downgrade\_\*\_passthrough

These passthroughs run jsCore tests while
[running the setFCV command in the background](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/buildscripts/resmokelib/testing/hooks/fcv_upgrade_downgrade.py#L).
The purpose of these passthroughs is to make sure jsCore operations that run concurrently with the
setFCV command behave correctly.

### FSM workloads

These include (e.g.
[drop_database_sharded_setFCV.js](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/jstests/concurrency/fsm_workloads/ddl/drop_database/drop_database_sharded_setFCV.js#L),
[random_ddl_crud_setFCV_operations.js](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/jstests/concurrency/fsm_workloads/crud/random_ddl_crud_setFCV_operations.js#L),
[random_ddl_setFCV_operations.js](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/jstests/concurrency/fsm_workloads/ddl/random_ddl/random_ddl_setFCV_operations.js#L),
[random_internal_transactions_setFCV_operations.js](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/jstests/concurrency/fsm_workloads/txns/internal_transactions/random_internal_transactions_setFCV_operations.js#L]))
These FSM workloads test random operations while concurrently changing the FCV.

These differ from the fcv_upgrade\_\_downgrade\_\*\_passthroughs in that they randomize operations
to increase the test coverage area, and they run a large amount of these operations in parallel,
which jsCore tests generally do not do. This gives increased coverage of how certain operations
interact with each other, more than what we'd see from a passthrough suite. This is why it's
especially helpful to add concurrency tests of high risk operations like DDL ops. Additionally, they
are able to test operations that may not be tested in jsCore. For example, a new project should
consider adding a setFCV FSM workload or modifying an existing workload if it adds a new DDL command
that has interactions with FCV, and is not tested in any jsCore tests.

### Testing IFR feature flags

When adding a feature that uses an IFR flag, consider adding a test to validate that runtime changes
to the flag do not cause undesirable behavior when executed concurrently with operations that use
the feature. An FSM workload is a good fit for this scenario, because it can interleave test
operations with `setParameter` commands (for toggling the flag) on multiple threads.

No special considerations are needed for the common case where an IFR flag state does not change at
runtime. Test coverage for `in-development` IFR flags is included by the "all feature flags"
variants. Additionally, the "all non-rollback feature flags" and "roll back incremental feature
flags" variants run tests with `rollout` IFR flags _disabled_ to ensure that it remains safe to turn
features back off in the event that an IFR deployment needs to be rolled back. The former variant
tests the server with all non-`rollout` flags turned on, and the latter only turns on release
features (IFR flags in the `release` state and non-IFR flags with `default: true`). There is no test
coverage for disabling IFR flags in the `release` state, because that is not a supported
configuration.

### Implicit mixed binary version testing

Generated suites that use the `useRandomBinVersionsWithinReplicaSet` variable (e.g.
[replica_sets_last_lts](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/buildscripts/resmokeconfig/matrix_suites/overrides/multiversion.yml#L202))
or `mixed_bin_versions` are suites that run with mixed binary versions. See more info
[here](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/docs/evergreen-testing/multiversion.md#explicit-and-implicit-multiversion-suites).
Note that this means that the cluster will be running on the
[downgraded (last-lts or last-continuous) FCV.](https://github.com/mongodb/mongo/blob/276ad8c4597134b610f22760a6ff6c480273f5af/jstests/libs/replsettest.js#L1239-L1271)
The purpose of these suites is to make sure that the server works correctly when communicating with
nodes on different binaries during upgrade/downgrade. For example, a bug that could be caught by
these suites is if a project made a change to the oplog format that was not feature flagged
correctly. This could cause nodes on a lower binary version to not be able to read oplog entries
that were generated from a node on a higher binary version and crash, which would be caught by these
types of suites.
