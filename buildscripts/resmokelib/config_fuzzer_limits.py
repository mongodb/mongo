"""Minimum and maximum dictionary declarations for the different randomized parameters (mongod, mongos, and flow control)."""

config_fuzzer_params = {
    "mongod": {
        "analyzeShardKeySplitPointExpirationSecs": {"min": 1, "max": 300},
        # minSnapshotHistoryWindowInSeconds has two sources of randomization (rng.randint(1, 10) * rng.choice(mongod_param_choices["minSnapshotHistoryWindowInSeconds"]))
        # You need to manually update minSnapshotHistoryWindowInSeconds min and max in the case that you change either randomized choices
        "minSnapshotHistoryWindowInSeconds": {"min": 30, "max": 600},
        "chunkMigrationConcurrency": {"choices": [1, 4, 16], "min": 1, "max": 16},
        "logicalSessionRefreshMillis": {
            "choices": [100, 1000, 10000, 100000],
            "min": 100,
            "max": 100000,
        },
        # maxNumberOfTransactionOperationsInSingleOplogEntry has two sources of randomization (rng.randint(1, 10) * rng.choice(mongod_param_choices["maxNumberOfTransactionOperationsInSingleOplogEntry"]))
        # You need to manually update maxNumberOfTransactionOperationsInSingleOplogEntry min and max in the case that you change either randomized choices
        "maxNumberOfTransactionOperationsInSingleOplogEntry": {
            "choices": [100, 1000, 10000, 100000],
            "min": 100,
            "max": 100000,
        },
        "enableAutoCompaction": {
            "choices": [True, False],
        },
        "initialServiceExecutorUseDedicatedThread": {
            "choices": [True, False],
        },
        "disableLogicalSessionCacheRefresh": {
            "choices": [True, False],
        },
        "wiredTigerStressConfig": {
            "choices": [True, False],
        },
        "oplogFetcherUsesExhaust": {
            "choices": [True, False],
        },
        "storageEngineConcurrencyAdjustmentAlgorithm": {
            "choices": ["throughputProbing", "fixedConcurrentTransactions"],
        },
        # For `initialSyncSourceReadPreference`, the option `secondary` is excluded from the fuzzer
        # because the generated mongod parameters are used for every node in the replica set, so the
        # secondaries in the replica set will not be able to find a valid sync source.
        "initialSyncSourceReadPreference": {
            "choices": ["nearest", "primary", "primaryPreferred", "secondaryPreferred"],
        },
        "initialSyncMethod": {"choices": ["fileCopyBased", "logical"]},
        "syncdelay": {"min": 15, "max": 180},
        "wiredTigerCursorCacheSize": {"min": -100, "max": 100},
        "wiredTigerSessionCloseIdleTimeSecs": {"min": 0, "max": 300},
        "storageEngineConcurrencyAdjustmentIntervalMillis": {"min": 10, "max": 1000},
        "throughputProbingStepMultiple": {"min": 0.1, "max": 0.5},
        "throughputProbingInitialConcurrency": {"min": 4, "max": 128},
        "throughputProbingMinConcurrency": {"min": 4, "max": "throughputProbingInitialConcurrency"},
        "throughputProbingMaxConcurrency": {
            "min": "throughputProbingInitialConcurrency",
            "max": 128,
        },
        "throughputProbingReadWriteRatio": {"min": 0, "max": 1},
        "throughputProbingConcurrencyMovingAverageWeight": {"min": 0.0, "max": 1.0},
        "wiredTigerConcurrentWriteTransactions": {"min": 5, "max": 32},
        "wiredTigerConcurrentReadTransactions": {"min": 5, "max": 32},
        "wiredTigerSizeStorerPeriodicSyncHits": {"min": 1, "max": 100000},
        "wiredTigerSizeStorerPeriodicSyncPeriodMillis": {"min": 1, "max": 60000},
        "replWriterThreadCount": {"min": 1, "max": 256},
        # The actual maximum of `replBatchLimitOperations` is 1000 * 1000 but this range doesn't work
        # for WINDOWS DEBUG, so that maximum is multiplied by 0.2, which is still a lot more than the
        # default value of 5000. The reason why the full range [1, 1000*1000] doesn't work on WINDOWS
        # DEBUG seems to be because it would wait for the batch to fill up to the batch limit
        # operations, but when that number is too high it would just time out before reaching the
        # batch limit operations.
        "replBatchLimitOperations": {"min": 1, "max": 0.2 * 1000 * 1000},
        "replBatchLimitBytes": {"min": 16 * 1024 * 1024, "max": 100 * 1024 * 1024},
        "queryAnalysisWriterMaxMemoryUsageBytes": {"min": 1024 * 1024, "max": 1024 * 1024 * 100},
        "internalQueryExecYieldIterations": {"min": 1, "max": 1000},
        "internalQueryExecYieldPeriodMS": {"min": 1, "max": 100},
        "mirrorReads": {"samplingRate": {"min": 0.0, "max": 0.01}},
        "flowControlTargetLagSeconds": {"min": 1, "max": 1000},
        "flowControlThresholdLagPercentage": {"min": 0.0, "max": 1.0},
        "flowControlMaxSamples": {"min": 1, "max": 1000 * 1000},
        "flowControlSamplePeriod": {"min": 1, "max": 1000 * 1000},
        "flowControlMinTicketsPerSecond": {"min": 1, "max": 10 * 1000},
        "enableFlowControl": {"choices": [True, False]},
    },
    "mongos": {
        "initialServiceExecutorUseDedicatedThread": {"choices": [True, False]},
        "opportunisticSecondaryTargeting": {"choices": [True, False]},
    },
}
