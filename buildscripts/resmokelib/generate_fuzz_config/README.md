# Config Fuzzer

This is a testing feature of the mongod and mongos, built into resmoke.py!

The config fuzzer is a resmoke feature that randomizes various server parameters of both mongod and mongos on startup. These fuzzed parameters should not affect the correctness of any tests. Therefore, the config fuzzer can be enabled for any test or suite run with resmoke to ensure the database is resilient to abnormal server configurations.

More information can be displayed in the resmoke --help output:

```
$ resmoke run --help
usage: Resmoke is MongoDB's correctness testing orchestrator.
### trimmed ###
  --fuzzMongodConfigs MODE
                        Randomly chooses mongod parameters that were not specified. Use 'stress' to fuzz all configs including stressful storage configurations that may significantly slow down the server. Use
                        'normal' to only fuzz non-stressful configurations.
  --fuzzMongosConfigs MODE
                        Randomly chooses mongos parameters that were not specified
  --fuzzRuntimeParams
                        Starts a hook that periodically updates shard and router server parameters while tests run
  --configFuzzSeed PATH
                        Sets the seed used by mongod and mongos config fuzzers
```

The bulk of the fuzzing logic is in [mongo_fuzzer_configs.py](./mongo_fuzzer_configs.py).

## How does it work?

The config fuzzer assigns random values to various tunable parameters. Server parameters and their ranges are specified manually by developers and are not discovered automatically in any way.

When the above resmoke flags are used, the [plugin](./plugin.py) implicitly enables the [FuzzRuntimeParameters](../../../buildscripts/resmokelib/testing/hooks/fuzz_runtime_parameters.py) hook for testing.

## Where and When does it run on evergreen?

The config fuzzer is represented as a handful of evergreen tasks with "_config_fuzzer_" in the name. Search "config_fuzzer" in the [etc/](../../../etc) directory to find all the evergreen tasks.

Arguably the simplest evergreen task, `config_fuzzer_jsCore`, runs the "core" (i.e. `jstests/core`) resmoke suite with the config fuzzer parameters to resmoke set, and excludes some incompatible tests ([src link](https://github.com/mongodb/mongo/blob/a2e7e83a135c3096de7f360b88de1b3cdc1caaf2/etc/evergreen_yml_components/tasks/resmoke/server_divisions/durable_transactions_and_availability/tasks.yml#L1956-L1975)). Here is a sampling of some of the task names:

- `config_fuzzer_concurrency_replication`
- `config_fuzzer_concurrency_sharded_replication`
- `config_fuzzer_stress_concurrency_replication`

## Reproducing a config fuzzer failure

In the Evergreen task view, click on the Logs tab, then Task Logs, and open in Parsely. Search for "Fuzzed" ([source link](https://github.com/mongodb/mongo/blob/ca1c935aca43ca2e028507e2a878d4e12f50355b/buildscripts/resmokelib/run/__init__.py#L352-L366)). The output will look similar to this:

<details>
<summary>Logs</summary>

```
[2024/05/14 10:24:22.828] [resmoke] 17:24:22.828Z Fuzzed mongodSetParameters:
[2024/05/14 10:24:22.828] analyzeShardKeySplitPointExpirationSecs: 82, min: 1, max: 300
[2024/05/14 10:24:22.828] chunkMigrationConcurrency: 16, min: 1, max: 16, options: [1, 4, 16]
[2024/05/14 10:24:22.828] defaultConfigCommandTimeoutMS: 90000
[2024/05/14 10:24:22.829] disableLogicalSessionCacheRefresh: False, options: [True, False]
[2024/05/14 10:24:22.829] enableAutoCompaction: False, options: [True, False]
[2024/05/14 10:24:22.829] enableFlowControl: True, options: [True, False]
[2024/05/14 10:24:22.829] flowControlMaxSamples: 208255, min: 1, max: 1000000
[2024/05/14 10:24:22.829] flowControlMinTicketsPerSecond: 4804, min: 1, max: 10000
[2024/05/14 10:24:22.829] flowControlSamplePeriod: 362413, min: 1, max: 1000000
[2024/05/14 10:24:22.829] flowControlTargetLagSeconds: 765, min: 1, max: 1000
[2024/05/14 10:24:22.829] flowControlThresholdLagPercentage: 0.13910090766883965, min: 0.0, max: 1.0
[2024/05/14 10:24:22.829] initialServiceExecutorUseDedicatedThread: True, options: [True, False]
[2024/05/14 10:24:22.829] initialSyncMethod: fileCopyBased, options: ['fileCopyBased', 'logical']
[2024/05/14 10:24:22.829] initialSyncSourceReadPreference: primaryPreferred, options: ['nearest', 'primary', 'primaryPreferred', 'secondaryPreferred']
[2024/05/14 10:24:22.829] internalQueryExecYieldIterations: 275, min: 1, max: 1000
[2024/05/14 10:24:22.829] internalQueryExecYieldPeriodMS: 61, min: 1, max: 100
[2024/05/14 10:24:22.829] logicalSessionRefreshMillis: 10000, min: 100, max: 100000, options: [100, 1000, 10000, 100000]
[2024/05/14 10:24:22.829] maxNumberOfTransactionOperationsInSingleOplogEntry: 3000, min: 100, max: 100000, options: [100, 1000, 10000, 100000]
[2024/05/14 10:24:22.829] minSnapshotHistoryWindowInSeconds: 494, min: 30, max: 600
[2024/05/14 10:24:22.829] mirrorReads: {'samplingRate': 0.12586091457924442}
[2024/05/14 10:24:22.829] oplogFetcherUsesExhaust: True, options: [True, False]
[2024/05/14 10:24:22.829] queryAnalysisWriterMaxMemoryUsageBytes: 47409830, min: 1048576, max: 104857600
[2024/05/14 10:24:22.829] receiveChunkWaitForRangeDeleterTimeoutMS: 300000
[2024/05/14 10:24:22.829] replBatchLimitBytes: 34729086, min: 16777216, max: 104857600
[2024/05/14 10:24:22.829] replBatchLimitOperations: 102691, min: 1, max: 200000.0
[2024/05/14 10:24:22.829] replWriterThreadCount: 171, min: 1, max: 256
[2024/05/14 10:24:22.829] storageEngineConcurrencyAdjustmentAlgorithm: throughputProbing, options: ['throughputProbing', 'fixedConcurrentTransactions']
[2024/05/14 10:24:22.829] storageEngineConcurrencyAdjustmentIntervalMillis: 777, min: 10, max: 1000
[2024/05/14 10:24:22.829] syncdelay: 111, min: 15, max: 180
[2024/05/14 10:24:22.829] throughputProbingConcurrencyMovingAverageWeight: 0.10355777691164192, min: 0.0, max: 1.0
[2024/05/14 10:24:22.829] throughputProbingInitialConcurrency: 91, min: 4, max: 128
[2024/05/14 10:24:22.829] throughputProbingMaxConcurrency: 94, min: throughputProbingInitialConcurrency, max: 128
[2024/05/14 10:24:22.829] throughputProbingMinConcurrency: 10, min: 4, max: throughputProbingInitialConcurrency
[2024/05/14 10:24:22.830] throughputProbingReadWriteRatio: 0.04082425476215046, min: 0, max: 1
[2024/05/14 10:24:22.830] throughputProbingStepMultiple: 0.18331313456713244, min: 0.1, max: 0.5
[2024/05/14 10:24:22.830] wiredTigerConcurrentReadTransactions: 13, min: 5, max: 32
[2024/05/14 10:24:22.830] wiredTigerConcurrentWriteTransactions: 29, min: 5, max: 32
[2024/05/14 10:24:22.830] wiredTigerCursorCacheSize: 51, min: -100, max: 100
[2024/05/14 10:24:22.830] wiredTigerSessionCloseIdleTimeSecs: 240, min: 0, max: 300
[2024/05/14 10:24:22.830] wiredTigerSizeStorerPeriodicSyncHits: 86240, min: 1, max: 100000
[2024/05/14 10:24:22.830] wiredTigerSizeStorerPeriodicSyncPeriodMillis: 19057, min: 1, max: 60000
[2024/05/14 10:24:22.830] wiredTigerStressConfig: False, options: [True, False]
[2024/05/14 09:35:21.273] [resmoke] 16:35:21.271Z Fuzzed wiredTigerConnectionString:
[2024/05/14 09:35:21.273] debug_mode.eviction: true
[2024/05/14 09:35:21.273] debug_mode.realloc_exact: false
[2024/05/14 09:35:21.273] debug_mode.rollback_error: 0
[2024/05/14 09:35:21.273] debug_mode.slow_checkpoint: false
[2024/05/14 09:35:21.273] eviction_checkpoint_target: 15, min: 1, max: 99
[2024/05/14 09:35:21.273] eviction_dirty_target: 209625542
[2024/05/14 09:35:21.273] eviction_dirty_trigger: 217257523
[2024/05/14 09:35:21.273] eviction_target: 72, min: 50, max: 95
[2024/05/14 09:35:21.273] eviction_trigger: 95, min: 1, max: 99
[2024/05/14 09:35:21.273] eviction_updates_target: 106058331
[2024/05/14 09:35:21.273] eviction_updates_trigger: 174509401
[2024/05/14 09:35:21.273] file_manager.close_handle_minimum: 350, min: 0, max: 1000
[2024/05/14 09:35:21.273] file_manager.close_idle_time: 56
[2024/05/14 09:35:21.273] file_manager.close_scan_interval: 73, min: 1, max: 100
[2024/05/14 09:35:21.273] [resmoke] 16:35:21.271Z configFuzzSeed:
[2024/05/14 09:35:21.273] 5583430894313922699
[2024/05/14 09:35:30.555] [resmoke] 16:35:30.554Z resmoke.py invocation for local usage:  buildscripts/resmoke.py run --suites=concurrency --continueOnFailure --excludeWithAnyTags=does_not_support_config_fuzzer --excludeWithAnyTags=incompatible_with_amazon_linux,requires_ldap_pool,requires_external_data_source,incompatible_with_atlas_environment --jobs=4 --shuffle --runAllFeatureFlagTests --storageEngineCacheSizeGB=1 --fuzzMongodConfigs=normal --configFuzzSeed=5583430894313922699
```

</details>

The log line starting with "resmoke.py invocation for local usage" and the one with "configFuzzSeed" provide an option `--configFuzzSeed=5583430894313922699` that can be used to generate the same fuzzed server parameters locally in resmoke.

## Running the config fuzzer locally

Before running the Resmoke config fuzzer command, you need to obtain the necessary binaries. You can download them from the "Files" section of the `archive_dist_test` task in Evergreen (e.g., binaries from the `amazon2-arm64-compile` variant). Alternatively, if you don't require those specific binaries, you can use `db-contrib-tool` to download the binaries (e.g., by running `db-contrib-tool setup-repro-env master`).

To re-run a command locally that failed through the config fuzzer, you can navigate to the specific test that failed, and under files you can find a name titled "Resmoke.py Invocation for Local Usage". If you are replicating an older config fuzzer invocation, remove the command line argument "`--installDir=dist-test/bin`". A simple example command is shown below:

```
buildscripts/resmoke.py run jstests/noPassthrough/bulk_write_w0.js \
  --fuzzMongodConfigs=normal \
  --fuzzMongosConfigs=normal \
  --configFuzzSeed=7956511060361033919
```

It is easiest to pipe the output to another text file and then to analyze the output through there. The format of the file is slightly different, as you will not be able to explicitly look up Fuzzed, but you can look up one of the fuzzed config parameters to find the list of fuzzed config parameter settings. A subset of a log from running the above command on [this version](https://github.com/mongodb/mongo/commit/856e4ecd8612b19c8ba281cf23450d74b5838650) of master yields is the following:

```
js_test:bulk_write_w0] Skip waiting to connect to node with pid=2522712, port=20040
[js_test:bulk_write_w0] ReplSetTest start skip waiting for a connection to node 0
[js_test:bulk_write_w0] ReplSetTest waiting for an initial connection to node 0
[js_test:bulk_write_w0] d20040| {"t":{"$date":"2024-04-29T13:42:25.030Z"},"s":"W",  "c":"CONTROL",  "id":636300,  "ctx":"main","msg":"Use of deprecated server parameter name","attr":{"deprecatedName":"wiredTigerConcurrentReadTransactions","canonicalName":"storageEngineConcurrentReadTransactions"}}
[js_test:bulk_write_w0] d20040| {"t":{"$date":"2024-04-29T13:42:25.030Z"},"s":"W",  "c":"CONTROL",  "id":636300,  "ctx":"main","msg":"Use of deprecated server parameter name","attr":{"deprecatedName":"wiredTigerConcurrentWriteTransactions","canonicalName":"storageEngineConcurrentWriteTransactions"}}
[js_test:bulk_write_w0] d20040| {"t":{"$date":"2024-04-29T13:42:25.030Z"},"s":"I",  "c":"CONTROL",  "id":5760901, "ctx":"main","msg":"Applied --setParameter options","attr":{"serverParameters":{"analyzeShardKeySplitPointExpirationSecs":{"default":300,"value":261},"backtraceLogFile":{"default":"","value":"/data/db/job0/mongorunner/mwugns392w418okq0z24f1714398141238.stacktrace"},"chunkMigrationConcurrency":{"default":1,"value":4},"coordinateCommitReturnImmediatelyAfterPersistingDecision":{"default":false,"value":false},"defaultConfigCommandTimeoutMS":{"default":30000,"value":90000},"disableLogicalSessionCacheRefresh":{"default":false,"value":true},"disableTransitionFromLatestToLastContinuous":{"default":true,"value":false},"enableDefaultWriteConcernUpdatesForInitiate":{"default":false,"value":true},"enableFlowControl":{"default":true,"value":false} ...
```

## Adding a new parameter to be fuzzed to the config fuzzer

There are two broad categories of parameters in the config fuzzer, that each have two sub-categories of parameters:

1. mongo parameters
   - mongod parameters
   - mongos parameters
2. WiredTiger parameters
   - eviction parameters
   - table parameters

### Adding new mongo parameters

Mongo parameters and their properties (e.g. min, max, default) are stored in [config_fuzzer_limits.py](./config_fuzzer_limits.py).

Below is a list of ways to fuzz configs which are supported without having to also change [mongo_fuzzer_configs.py](./mongo_fuzzer_configs.py).
Please ensure that you add it correctly to the `mongod` or `mongos` subdictionary.

You need to specify if your parameter should be fuzzed at runtime, startup, or both by declaring the `fuzz_at` key for the parameter. The `fuzz_at` key should be a list that can contain the values `startup`, `runtime`, or both. The eligible values are specified in the `set_at` keys of the corresponding `.idl` files.

For a parameter that is only fuzzed at startup, the fuzzer will generate a fuzzed value for the parameter and set it when starting up the server.

For a parameter fuzzed at runtime, the fuzzer will generate a fuzzed value for the parameter while running the server based on a `period` key that is required for fuzzed runtime parameters.
The `period` key describes how often the parameter should be changed, in seconds. Every `period` seconds, the fuzzer will select a new random value for the parameter and use the setParameter command to update the value of the
parameter on every node in the cluster while the suite is running. This is perfomed by the [FuzzRuntimeParameters](../../../buildscripts/resmokelib/testing/hooks/fuzz_runtime_parameters.py) hook.

Let `choices = [choice1, choice2, ..., choiceN]` be an array of choices that the parameter can have as a value.
The parameters are added in order of priority chosen in the if-elif-else statement in `generate_normal_mongo_parameters()`
in [mongo_fuzzer_configs.py](./mongo_fuzzer_configs.py).
So, if you added the fields `default`, `min`, and `max` for a `param`, case 4 would get evaluated over case 5.

1. `param = rng.uniform(min, max)`

   Add:

   ```
   "param": {"min": min, "max": max, "isUniform": True}
   ```

2. `param = rng.randint([choices, rng.randint(min, max)])`

   Add:

   ```
   "param": {
       "min": <min of (min and choices)>,
       "max": <max of (max and choices)>,
       "lower_bound": lower_bound,
       "upper_bound": upper_bound,
       "choices": [choice1, choice2, ..., choiceN],
       "isRandomizedChoice": true
   }
   ```

3. `param = rng.choices(choices)`, where choices is an array

   Add:

   ```
   "param": {"choices": [choice1, choice2, ..., choiceN]}
   ```

4. `param = rng.randint(min, max)`

   Add:

   ```
   "param": {"min": min, "max": max}
   ```

5. `param = default`

   Add:

   ```
   "param": {"default": default}
   ```

   > Note: For the default case, please add the value `"fuzz_at": ["startup"]` (the default value gets set at "startup").

If you have a parameter that depends on another parameter being generated (see `throughputProbingInitialConcurrency` needing to be initialized before
`throughputProbingMinConcurrency` and `throughputProbingMaxConcurrency` as an example in [mongo_fuzzer_configs.py](./mongo_fuzzer_configs.py)) or behavior that
differs from the above cases, please do the following steps:

1. Add the parameter and the needed information about the parameters here (ensure to correctly add to the `mongod` or `mongos` sub-dictionary)

In [mongo_fuzzer_configs.py](./mongo_fuzzer_configs.py):

2. Add the parameter to `excluded_normal_parameters` in the `generate_mongod_parameters()` or `generate_mongos_parameters()`
3. Add the parameter's special handling in `generate_special_mongod_parameters()` or `generate_special_mongos_parameters()`

If you add a flow control parameter, please add the the parameter's name to `flow_control_params` in `generate_mongod_parameters`.

> Note: The main distinction between min/max vs. lower-bound/upper_bound is there is some transformation involving the lower and upper bounds,
> while the min/max should be the true min/max of the parameters. You should also include the true min/max of the parameter so this can be logged.
> If the min/max is not inclusive, this is added as a note above the parameter.

### Adding new WiredTiger parameters

WiredTiger parameters and their properties (e.g. min, max, default) are stored in [config_fuzzer_wt_limits.py](./config_fuzzer_wt_limits.py).

> These _can not_ be fuzzed with the [FuzzRuntimeParameters](../../../buildscripts/resmokelib/testing/hooks/fuzz_runtime_parameters.py) hook because they are only set on startup (these parameters are used in the wt configuration string).

Below is a list of ways to fuzz configs which are supported without having to also change [mongo_fuzzer_configs.py](./mongo_fuzzer_configs.py).

Please ensure that you add it correctly to the `wt` (eviction parameters) or `wt_table` subdictionary.

Let `choices = [choice1, choice2, ..., choiceN]` be an array of choices that the parameter can have as a value.

The parameters are added in order of priority chosen in the if-elif-else statement in `generate_normal_wt_parameters()` in
[mongo_fuzzer_configs.py](./mongo_fuzzer_configs.py).

1. `param = rng.choices(choices)`, where choices is an array

   Add:

   ```
   "param": {"choices": [choice1, choice2, ..., choiceN]}
   ```

   You can also add a "multiplier" key which multiplies the key by the multiplier value.

   ```
   param = rng.choice(choices) * multiplier
   ```

   Add:

   ```
   "param": {"choices": [choice1, choice2, ..., choiceN], "multiplier": multiplier}
   ```

2. `param = rng.randint(min, max)`

   Add:

   ```
   "param": {"min": min, "max": max}
   ```

If you have a parameter that depends on another parameter being generated (see `eviction_target` needing to be initialized before
`eviction_trigger` as an example in [mongo_fuzzer_configs.py](./mongo_fuzzer_configs.py)) or behavior that differs from the above cases,
please do the following step:

1. Add the parameter and the needed information about the parameters here (ensure to correctly add to the wt or wt_table sub-dictionary)

In mongo_fuzzer_configs.py:

2. Add the parameter to `excluded_normal_parameters` in the `generate_eviction_configs()` or `generate_table_configs()`
3. Add the parameter's special handling in `generate_special_eviction_configs()` or `generate_special_table_configs()`

> The main distinction between min/max vs. lower-bound/upper_bound is there is some transformation involving the lower and upper bounds,
> while the min/max should be the true min/max of the parameters. You should also include the true min/max of the parameter so this can be logged.
> If the min/max is not inclusive, this is added as a note above the parameter.

## Exclusions

- `jstests/libs/override_methods/config_fuzzer_incompatible_commands.js`
  - These commands are too impactful to run with the config fuzzer
- The `does_not_support_config_fuzzer` jstest tag
  - Tests with this tag may manually specify server parameters modified by the fuzzer or read global state that is modified in some way by the fuzzer.
  - Just because a test is failing does not mean it is incompatible with the config fuzzer.
