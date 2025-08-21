# Hooks

Hooks are a mechanism to run routines _around_ the tests, at the test content boundaries.

## Supported hooks

Specify any of the following as the `hooks` in your [Suite](../../../../buildscripts/resmokeconfig/suites/README.md) config:

- [`AnalyzeShardKeysInBackground`](./analyze_shard_key.py) - A hook for running `analyzeShardKey` commands while a test is running.
- [`AntithesisLogging`](./antithesis_logging.py) - Prints antithesis commands before & after test run.
- [`BackgroundInitialSync`](./initialsync.py) - Background Initial Sync
  - After every test, this hook checks if a background node has finished initial sync and if so validates it, tears it down, and restarts it.
  - This test accepts a parameter `n` that specifies a number of tests after which it will wait for replication to finish before validating and restarting the initial sync node.
  - This requires the ReplicaSetFixture to be started with `start_initial_sync_node=True`. If used at the same time as `CleanEveryN`, the `n` value passed to this hook should be equal to the `n` value for `CleanEveryN`.
- [`CheckClusterIndexConsistency`](./cluster_index_consistency.py) - Checks that indexes are the same across chunks for the same collections.
- [`CheckMetadataConsistencyInBackground`](./metadata_consistency) - Check the metadata consistency of a sharded cluster.
- [`CheckOrphansDeleted`](./orphans.py) - Check if the range deleter failed to delete any orphan documents.
- [`CheckReplChangeCollectionConsistency`](./change_collection_consistency.py) - Check that config.system.change_collection is consistent between the primary and secondaries for all tenants.
- [`CheckReplDBHashInBackground`](./dbhash_background.py) - A hook for comparing the dbhashes of all replica set members while a test is running.
- [`CheckReplDBHash`](./dbhash.py) - Check if the dbhashes match.
- [`CheckReplOplogs`](./oplog.py) - Check that `local.oplog.rs` matches on the primary and secondaries.
- [`CheckReplPreImagesConsistency`](./preimages_consistency.py) - Check that `config.system.preimages` is consistent between the primary and secondaries.
- [`CheckRoutingTableConsistency`](./routing_table_consistency.py) - Verifies the absence of corrupted entries in config.chunks and config.collections.
- [`CheckShardFilteringMetadata`](./shard_filtering_metadata.py) - Inspect filtering metadata on shards
- [`CleanEveryN`](./cleanup.py) - Restart the fixture after it has ran `n` tests.
- [`CleanupConcurrencyWorkloads`](./cleanup_concurrency_workloads.py) - Drop all databases, except those that have been excluded.
  - For concurrency tests that run on different DBs, drop all databases except ones in `exclude_dbs`. For tests that run on the same DB, drop all databases except ones in `exclude_dbs` and the DB used by the test/workloads. For tests that run on the same collection, drop all collections in all databases except for `exclude_dbs` and the collection used by the test/workloads.
  - On mongod-related fixtures, this will clear the dbpath
- [`ClusterParameter`](./cluster_parameter.py) - Sets the specified cluster server parameter.
- [`ContinuousAddRemoveShard`](./add_remove_shards.py) - Continuously adds and removes shards at regular intervals. If running with `configsvr` transitions, will transition in/out of config shard mode.
- [`ContinuousInitialSync`](./continuous_initial_sync.py) - Periodically initial sync nodes then step them up.
- [`ContinuousStepdown`](./stepdown.py) - regularly connect to replica sets and send a `replSetStepDown` command.
- [`ContinuousTransition`](./replicaset_transition_to_and_from_csrs.py) - connects to replica sets and transitions them from replica set to CSRS node in the background.
- [`DoReconfigInBackground`](./reconfig_background.py) - A hook for running a safe reconfig against a replica set while a test is running.
- [`DropSessionsCollection`](./drop_sessions_collection.py) - A hook for dropping and recreating config.system.sessions while tests are running.
- [`DropUserCollections`](./drop_user_collections.py) - Drops all user collections.
- [`EnableChangeStream`](./enable_change_stream.py) - Enable change stream hook class.
  - Enables change stream in the multi-tenant environment for the replica set and the sharded
    cluster.
- [`EnableSpuriousWriteConflicts`](./enable_spurious_write_conflicts.py) - Toggles write conflicts.
- [`FCVUpgradeDowngradeInBackground`](./fcv_upgrade_downgrade.py) - A hook to run background FCV upgrade and downgrade against test servers while a test is running.
- [`FuzzRuntimeParameters`](./fuzz_runtime_parameters.py) - Regularly connect to nodes and sends them a `setParameter` command; uses the [Config Fuzzer](../../../../buildscripts/resmokelib/generate_fuzz_config/README.md).
- [`FuzzRuntimeStress`](./fuzz_runtime_stress.py) - Test hook that periodically changes the amount of stress the system is experiencing.
- [`FuzzerRestoreSettings`](./fuzzer_restore_settings.py) - Cleans up unwanted changes from fuzzer.
- [`GenerateAndCheckPerfResults`](./generate_and_check_perf_results.py) - Combine JSON results from individual benchmarks and check their reported values against any thresholds set for them.
  - Combines test results from individual benchmark files to a single file. This is useful for generating the json file to feed into the Evergreen performance visualization plugin.
- [`HelloDelays`](./hello_failures.py) - Sets Hello fault injections.
- [`IntermediateInitialSync`](./initialsync.py) - Intermediate Initial Sync
  - This hook accepts a parameter `n` that specifies a number of tests after which it will start up a node to initial sync, wait for replication to finish, and then validate the data.
  - This requires the ReplicaSetFixture to be started with 'start_initial_sync_node=True'.
- [`LagOplogApplicationInBackground`](./secondary_lag.py) - Toggles secondary oplog application lag.
- [`LibfuzzerHook`](./cpp_libfuzzer.py) - Merges inputs after a fuzzer run.
- [`MagicRestoreEveryN`](./magic_restore.py) - Open a backup cursor and run magic restore process after `n` tests have run.
  - Requires the use of `MagicRestoreFixture`.
- [`PeriodicKillSecondaries`](./periodic_kill_secondaries.py) - Periodically kills the secondaries in a replica set.
  - Also verifies that the secondaries can reach the SECONDARY state without having connectivity to the primary after an unclean shutdown.
- [`PeriodicStackTrace`](./periodic_stack_trace.py) - Test hook that sends the stacktracing signal to mongo processes at randomized intervals.
- [`QueryableServerHook`](./queryable_server_hook.py) - Starts the queryable server before each test for queryable restores. Restarts the queryable server between tests.
- [`RunChangeStreamsInBackground`](./change_streams.py) - Run in the background full cluster change streams while a test is running. Open and close the change stream every `1..10` tests (random using `config.RANDOM_SEED`).
- [`RunDBCheckInBackground`](./dbcheck_background.py) - A hook for running `dbCheck` on a replica set while a test is running.
  - This includes dbhashes for all non-local databases and non-replicated system collections that match on the primary and secondaries.
  - It also will check the performance results against any thresholds that are set for each benchmark. If no thresholds are set for a test, this hook should always pass.
- [`RunQueryStats`](./run_query_stats.py) - Runs `$queryStats` after every test, and clears the query stats store before every test.
- [`SimulateCrash`](./simulate_crash.py) - A hook to simulate crashes.
- [`ValidateCollections`](./validate.py) - Run full validation.
- [`ValidateCollectionsInBackground`](./validate_background.py) - A hook to run background collection validation against test servers while a test is running.
  - This will run on all collections in all databases on every stand-alone node, primary replica-set node, or primary shard node.
- [`ValidateDirectSecondaryReads`](./validate_direct_secondary_reads.py) - Only supported in suites that use `ReplicaSetFixture`.
  - To be used with `set_read_preference_secondary.js` and `implicit_enable_profiler.js` in suites that read directly from secondaries in a replica set. Check the profiler collections of all databases at the end of the suite to verify that each secondary only ran the read commands it got directly from the shell.
- [`WaitForReplication`](./wait_for_replication.py) - Wait for replication to complete.

## Interfaces

All hooks inherit from the [`buildscripts.resmokelib.testing.hooks.interface.Hook`](./interface.py) parent class and can override any subset of the following empty base methods:

- `before_suite`
- `before_test`
- `after_test`
- `after_suite`

At least 1 base method must be overridden, otherwise the hook will not do anything at all. During test suite execution, each hook runs its custom logic in the respective scenarios. Some customizable tasks that hooks can perform include: _validating data, deleting data, performing cleanup_, etc.

- [`BGHook`](./bghook.py) - A hook that repeatedly calls `run_action()` in a background thread for the duration of the test suite.
- [`DataConsistencyHook`](./jsfile.py) - A hook for running a static JavaScript file that checks data consistency of the server.
  - If the mongo shell process running the JavaScript file exits with a non-zero return code, then an `errors.ServerFailure` exception is raised to cause resmoke.py's test execution to stop.
- [`Hook`](./interface.py) - Common interface all Hooks will inherit from.
- [`JSHook`](./jsfile.py) - A hook interface with a static JavaScript file to execute.
- [`PerClusterDataConsistencyHook`](./jsfile.py) - A hook that runs on each independent cluster of the fixture.
  - The independent cluster itself may be another fixture.
