"""Minimum and maximum dictionary declarations for the different randomized parameters (mongod and mongos)."""

"""
How to add a fuzzed mongod/mongos parameter:

Below is a list of ways to fuzz configs which are supported without having to also change buildscripts/resmokelib/mongo_fuzzer_configs.py.
Please ensure that you add it correctly to the "mongod" or "mongos" subdictionary.

You need to specify if your parameter should be fuzzed at runtime, startup, or both by declaring the 'fuzz_at' key for the parameter. The 'fuzz_at' key should be a list that can contain the values "startup", "runtime", or both.
For a parameter that is only fuzzed at startup, the fuzzer will generate a fuzzed value for the parameter and set it when starting up the server.
For a parameter fuzzed at runtime, the fuzzer will generate a fuzzed value for the parameter while running the server based on a 'period' key that is required for fuzzed runtime parameters.
The 'period' key describes how often the parameter should be changed, in seconds. Every 'period' seconds, the fuzzer will select a new random value for the parameter and use the setParameter command to update the value of the
parameter on every node in the cluster while the suite is running.

Let choices = [choice1, choice2, ..., choiceN] (an array of choices that the parameter can have as a value).
The parameters are added in order of priority chosen in the if-elif-else statement in generate_normal_mongo_parameters()
in buildscripts/resmokelib/mongo_fuzzer_configs.py.
So, if you added the fields "default", "min", and "max" for a "param", case 4 would get evaluated over case 5.

1. param = rng.uniform(min, max)
    Add:
    “param”: {“min”: min, “max”: max, "isUniform": True}

2. param = rng.randint([choices, rng.randint(min, max)])
    Add:
    "param": {"min": <min of (min and choices)>, "max": <max of (max and choices)>, "lower_bound": lower_bound, "upper_bound": upper_bound, "choices": [choice1, choice2, ..., choiceN], "isRandomizedChoice": true}

3. param = rng.choices(choices), where choices is an array
    Add:
    "param": {"choices": [choice1, choice2, ..., choiceN]}

4. param = rng.randint(min, max)
    Add:
    “param”: {“min”: min, “max”: max}

5. param = default
    Add:
    "param": {"default": default}
    Note: For the default case, please add the value "fuzz_at": ["startup"] (the default value gets set at "startup").

If you have a parameter that depends on another parameter being generated (see throughputProbingInitialConcurrency needing to be initialized before
throughputProbingMinConcurrency and throughputProbingMaxConcurrency as an example in buildscripts/resmokelib/mongo_fuzzer_configs.py) or behavior that
differs from the above cases, please do the following steps:
1. Add the parameter and the needed information about the parameters here (ensure to correctly add to the mongod or mongos sub-dictionary)

In buildscripts/resmokelib/mongo_fuzzer_configs.py:
2. Add the parameter to excluded_normal_parameters in the generate_mongod_parameters() or generate_mongos_parameters()
3. Add the parameter's special handling in generate_special_mongod_parameters() or generate_special_mongos_parameters()

If you add a flow control parameter, please add the the parameter's name to flow_control_params in generate_mongod_parameters.

Note: The main distinction between min/max vs. lower-bound/upper_bound is there is some transformation involving the lower and upper bounds,
while the min/max should be the true min/max of the parameters. You should also include the true min/max of the parameter so this can be logged.
If the min/max is not inclusive, this is added as a note above the parameter.
"""

config_fuzzer_params = {
    "mongod": {
        "analyzeShardKeySplitPointExpirationSecs": {"min": 1, "max": 300, "fuzz_at": ["startup"]},
        "analyzeShardKeyMaxNumStaleVersionRetries": {
            "min": 0,
            "max": 10,
            "default": 0,
            "fuzz_at": ["startup"],
        },
        "chunkMigrationConcurrency": {
            "choices": [1, 4, 16],
            "min": 1,
            "max": 16,
            "fuzz_at": ["startup"],
        },
        "collectionSamplingLogIntervalSeconds": {
            "min": 5,
            "max": 15,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "disableLogicalSessionCacheRefresh": {"choices": [True, False], "fuzz_at": ["startup"]},
        "enableAutoCompaction": {"choices": [True, False], "fuzz_at": ["startup"]},
        "ingressAdmissionControllerTicketPoolSize": {
            "choices": [100, 1_000, 10_000, 100_000, 1_000_000],
            "lower_bound": 100,
            "upper_bound": 1_000_000,
            "min": 100,
            "max": 1_000_000,
            "isRandomizedChoice": True,
            "period": 5,
            "fuzz_at": ["runtime"],
        },
        "enableTemporarilyUnavailableExceptions": {
            "choices": [True, False],
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "indexBuildMinAvailableDiskSpaceMB": {
            "min": 250,
            "max": 750,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "initialSyncMethod": {"choices": ["fileCopyBased", "logical"], "fuzz_at": ["startup"]},
        # For `initialSyncSourceReadPreference`, the option `secondary` is excluded from the fuzzer
        # because the generated mongod parameters are used for every node in the replica set, so the
        # secondaries in the replica set will not be able to find a valid sync source.
        "initialSyncSourceReadPreference": {
            "choices": ["nearest", "primary", "primaryPreferred", "secondaryPreferred"],
            "fuzz_at": ["startup"],
        },
        "internalInsertMaxBatchSize": {
            "min": 1,
            "max": 750,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        # internalQueryExecYieldIterations takes a weighted random choice with a rand int generated betwen the lower_bound and upper_bound and 1.
        "internalQueryExecYieldIterations": {
            "min": 1,
            "max": 1000,
            "lower_bound": 1,
            "upper_bound": 1000,
            "fuzz_at": ["startup"],
        },
        "internalQueryExecYieldPeriodMS": {"min": 1, "max": 100, "fuzz_at": ["startup"]},
        "internalQueryFindCommandBatchSize": {"min": 1, "max": 500, "fuzz_at": ["startup"]},
        "journalCommitInterval": {
            "min": 50,
            "max": 250,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "lockCodeSegmentsInMemory": {"choices": [True, False], "fuzz_at": ["startup"]},
        "logicalSessionRefreshMillis": {
            "choices": [100, 1000, 10_000, 100_000],
            "min": 100,
            "max": 100_000,
            "fuzz_at": ["startup"],
        },
        "maxNumActiveUserIndexBuilds": {
            "min": 1,
            "max": 5,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        # maxNumberOfTransactionOperationsInSingleOplogEntry has two sources of randomization (rng.randint(1, 10) * rng.choice(mongod_param_choices["maxNumberOfTransactionOperationsInSingleOplogEntry"]))
        # You need to manually update maxNumberOfTransactionOperationsInSingleOplogEntry min and max in the case that you change either randomized choices.
        "maxNumberOfTransactionOperationsInSingleOplogEntry": {
            "choices": [1, 10, 100],
            "min": 1,
            "max": 1000,
            "fuzz_at": ["startup"],
        },
        "operationMemoryPoolBlockMaxSizeKB": {
            "min": 1024,
            "max": 2048,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "oplogFetcherUsesExhaust": {"choices": [True, False], "fuzz_at": ["startup"]},
        # The actual maximum of `replBatchLimitOperations` is 1000 * 1000 but this range doesn't work
        # for WINDOWS DEBUG, so that maximum is multiplied by 0.2, which is still a lot more than the
        # default value of 5000. The reason why the full range [1, 1000*1000] doesn't work on WINDOWS
        # DEBUG seems to be because it would wait for the batch to fill up to the batch limit
        # operations, but when that number is too high it would just time out before reaching the
        # batch limit operations.
        "replBatchLimitOperations": {"min": 1, "max": 0.2 * 1000 * 1000, "fuzz_at": ["startup"]},
        "replBatchLimitBytes": {
            "min": 16 * 1024 * 1024,
            "max": 100 * 1024 * 1024,
            "fuzz_at": ["startup"],
        },
        "replWriterThreadCount": {"min": 1, "max": 256, "fuzz_at": ["startup"]},
        "storageEngineConcurrencyAdjustmentAlgorithm": {
            "choices": ["throughputProbing", "fixedConcurrentTransactions"],
            "fuzz_at": ["startup"],
        },
        "storageEngineConcurrencyAdjustmentIntervalMillis": {
            "min": 10,
            "max": 1000,
            "fuzz_at": ["startup"],
        },
        "temporarilyUnavailableBackoffBaseMs": {"min": 1, "max": 1000, "fuzz_at": ["startup"]},
        "temporarilyUnavailableMaxRetries": {"min": 1, "max": 10, "fuzz_at": ["startup"]},
        # throughputProbingConcurrencyMovingAverageWeight is 1 - rng.random(), so the min is NOT inclusive.
        "throughputProbingConcurrencyMovingAverageWeight": {
            "min": 0.0,
            "max": 1.0,
            "fuzz_at": ["startup"],
        },
        "throughputProbingInitialConcurrency": {"min": 4, "max": 128, "fuzz_at": ["startup"]},
        "throughputProbingMinConcurrency": {
            "min": 4,
            "max": "throughputProbingInitialConcurrency",
            "fuzz_at": ["startup"],
        },
        "throughputProbingMaxConcurrency": {
            "min": "throughputProbingInitialConcurrency",
            "max": 128,
            "fuzz_at": ["startup"],
        },
        "throughputProbingReadWriteRatio": {
            "min": 0,
            "max": 1,
            "isUniform": True,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "throughputProbingStepMultiple": {
            "min": 0.1,
            "max": 0.5,
            "isUniform": True,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "transactionTooLargeForCacheThreshold": {
            "min": 0.5,
            "max": 1,
            "isUniform": True,
            "fuzz_at": ["startup"],
        },
        "wiredTigerStressConfig": {"choices": [True, False], "fuzz_at": ["startup"]},
        "wiredTigerCursorCacheSize": {"min": -100, "max": 100, "fuzz_at": ["startup"]},
        "wiredTigerSessionCloseIdleTimeSecs": {"min": 0, "max": 300, "fuzz_at": ["startup"]},
        "wiredTigerConcurrentReadTransactions": {"min": 5, "max": 32, "fuzz_at": ["startup"]},
        "wiredTigerConcurrentWriteTransactions": {"min": 5, "max": 32, "fuzz_at": ["startup"]},
        "wiredTigerSizeStorerPeriodicSyncHits": {"min": 1, "max": 100_000, "fuzz_at": ["startup"]},
        "wiredTigerSizeStorerPeriodicSyncPeriodMillis": {
            "min": 1,
            "max": 60_000,
            "fuzz_at": ["startup"],
        },
        "wiredTigerCheckpointCleanupPeriodSeconds": {
            "min": 60,
            "max": 600,  # This can be as high as 100k but we fuzz it to be small because we mostly perform 0 cleanups in testing.
            "fuzz_at": ["startup"],
        },
        "queryAnalysisWriterMaxMemoryUsageBytes": {
            "min": 1024 * 1024,
            "max": 1024 * 1024 * 100,
            "fuzz_at": ["startup"],
        },
        "mirrorReads": {
            "choices": [0, 0.25, 0.50, 0.75, 1.0],
            "min": 0,
            "max": 1,
            "fuzz_at": ["startup"],
        },
        # Flow control related parameters
        "enableFlowControl": {"choices": [True, False], "fuzz_at": ["startup"]},
        "flowControlTicketAdderConstant": {
            "min": 500,
            "max": 1000,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "flowControlDecayConstant": {
            "min": 0.1,
            "max": 1,
            "isUniform": True,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "flowControlFudgeFactor": {
            "min": 0.9,
            "max": 1,
            "isUniform": True,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "flowControlMaxSamples": {"min": 1, "max": 1000 * 1000, "fuzz_at": ["startup"]},
        "flowControlMinTicketsPerSecond": {
            "min": 1,
            "max": 10 * 1000,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "flowControlSamplePeriod": {
            "min": 1,
            "max": 1000 * 1000,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "flowControlTargetLagSeconds": {
            "min": 1,
            "max": 1000,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "flowControlTicketMultiplierConstant": {
            "min": 1.01,
            "max": 1.09,
            "isUniform": True,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        # flowControlThresholdLagPercentage is found by calling rng.random(), which means the max is NOT inclusive.
        "flowControlThresholdLagPercentage": {
            "min": 0.0,
            "max": 1.0,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "flowControlWarnThresholdSeconds": {
            "min": 5,
            "max": 15,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        # We need a higher timeout to account for test slowness
        "receiveChunkWaitForRangeDeleterTimeoutMS": {"default": 300_000, "fuzz_at": ["startup"]},
        "defaultConfigCommandTimeoutMS": {"default": 90_000, "fuzz_at": ["startup"]},
        "maxSizeOfBatchedInsertsForRenameAcrossDatabasesBytes": {
            "min": 2_097_152,
            "max": 6_291_456,
            "fuzz_at": ["startup"],
        },
        "maxNumberOfInsertsBatchInsertsForRenameAcrossDatabases": {
            "min": 1,
            "max": 1000,
            "fuzz_at": ["startup"],
        },
        # These parameters have a min, max, and a choice with one value because we first find rng.randint(min, max)
        # and then add this value to the choices array and call rng.choices(choices).
        "minSnapshotHistoryWindowInSeconds": {
            "choices": [300],
            "lower_bound": 30,
            "upper_bound": 600,
            "min": 30,
            "max": 600,
            "isRandomizedChoice": True,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "syncdelay": {
            "choices": [60],
            "lower_bound": 15,
            "upper_bound": 180,
            "min": 15,
            "max": 180,
            "isRandomizedChoice": True,
            "fuzz_at": ["startup"],
        },
        "connPoolMaxInUseConnsPerHost": {"min": 50, "max": 100, "fuzz_at": ["startup"]},
        "globalConnPoolIdleTimeoutMinutes": {"min": 1, "max": 10, "fuzz_at": ["startup"]},
        "ShardingTaskExecutorPoolMaxConnecting": {"min": 1, "max": 2, "fuzz_at": ["startup"]},
        "warmMinConnectionsInShardingTaskExecutorPoolOnStartup": {
            "choices": [True, False],
            "fuzz_at": ["startup"],
        },
        "oplogBatchDelayMillis": {"min": 0, "max": 50, "fuzz_at": ["startup"]},
        # Test hanging a random amount of time during DDL commits. This widens the window of
        # potential failure to have inconsistent CollectionCatalog instances with the WT snapshot.
        "failpoint.hangAfterPreCommittingCatalogUpdates": {
            "pauseEntireCommitMillis": {"min": 10, "max": 100},
            "fuzz_at": ["startup"],
        },
        "failpoint.hangBeforePublishingCatalogUpdates": {
            "pauseEntireCommitMillis": {"min": 10, "max": 100},
            "fuzz_at": ["startup"],
        },
        # Choose whether to shuffle the list command results or not.
        "failpoint.shuffleListCommandResults": {
            "choices": [{"mode": "off"}, {"mode": "alwaysOn"}],
            "fuzz_at": ["startup"],
        },
        "failpoint.enableSignalTesting": {
            "choices": [{"mode": "off"}, {"mode": "alwaysOn"}],
            "fuzz_at": ["startup"],
        },
        "enableDetailedConnectionHealthMetricLogLines": {
            "choices": [True, False],
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "diagnosticDataCollectionEnabled": {
            "choices": [True, False],
            "fuzz_at": ["startup"],
        },
        "diagnosticDataCollectionVerboseTCMalloc": {
            "choices": [True, False],
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
        "diagnosticDataCollectionEnableLatencyHistograms": {
            "choices": [True, False],
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
    },
    "mongos": {
        # We need a higher timeout to account for test slowness
        "defaultConfigCommandTimeoutMS": {"default": 90_000, "fuzz_at": ["startup"]},
        "internalQueryFindCommandBatchSize": {"min": 1, "max": 500, "fuzz_at": ["startup"]},
        "opportunisticSecondaryTargeting": {"choices": [True, False], "fuzz_at": ["startup"]},
        "ShardingTaskExecutorPoolReplicaSetMatching": {
            "choices": ["disabled", "matchBusiestNode", "matchPrimaryNode"],
            "fuzz_at": ["startup"],
        },
        "userCacheInvalidationIntervalSecs": {
            "min": 1,
            "max": 86400,
            "period": 5,
            "fuzz_at": ["runtime"],
        },
        "enableDetailedConnectionHealthMetricLogLines": {
            "choices": [True, False],
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "warmMinConnectionsInShardingTaskExecutorPoolOnStartup": {
            "choices": [True, False],
            "fuzz_at": ["startup"],
        },
        "diagnosticDataCollectionEnabled": {
            "choices": [True, False],
            "fuzz_at": ["startup"],
        },
        "diagnosticDataCollectionVerboseTCMalloc": {
            "choices": [True, False],
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
        "diagnosticDataCollectionEnableLatencyHistograms": {
            "choices": [True, False],
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
        "failpoint.enableSignalTesting": {
            "choices": [{"mode": "off"}, {"mode": "alwaysOn"}],
            "fuzz_at": ["startup"],
        },
    },
}

config_fuzzer_extra_configs = {
    "mongod": {
        "directoryperdb": {"choices": [True, False]},
        "wiredTigerDirectoryForIndexes": {"choices": [True, False]},
    },
    "mongos": {},
}
