"""Minimum and maximum dictionary declarations for the different randomized parameters (mongod and mongos)."""


"""
For context and maintenance, see:
https://github.com/10gen/mongo/blob/master/buildscripts/resmokelib/generate_fuzz_config/README.md#adding-new-mongo-parameters
"""

config_fuzzer_params = {
    "mongod": {
        "analyzeShardKeyNumRanges": {
            "min": 2,
            "max": 100,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "analyzeShardKeySplitPointExpirationSecs": {"min": 1, "max": 300, "fuzz_at": ["startup"]},
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
        "ingressConnectionEstablishmentRateLimiterEnabled": {
            "choices": [True, False],
            "period": 60,
            "fuzz_at": ["startup", "runtime"],
        },
        "ingressConnectionEstablishmentRatePerSec": {
            "min": 20,
            "max": 100_000,
            "period": 60,
            "fuzz_at": ["startup", "runtime"],
        },
        "ingressConnectionEstablishmentBurstCapacitySecs": {
            "min": 0.1,
            "max": 100_000,
            "isUniform": True,
            "period": 60,
            "fuzz_at": ["startup", "runtime"],
        },
        "ingressConnectionEstablishmentMaxQueueDepth": {
            "min": 100,
            "max": 100_000,
            "period": 60,
            "fuzz_at": ["startup", "runtime"],
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
        "initialSyncIndexBuildMemoryPercentage": {
            "min": 0,
            "max": 20,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "initialSyncIndexBuildMemoryMinMB": {
            "min": 50,
            "max": 1024,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "initialSyncIndexBuildMemoryMaxMB": {
            "min": 50,
            "max": 1024,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
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
        # Default value False; we are enabling more data integrity checks during timeseries compression.
        "performTimeseriesCompressionIntermediateDataIntegrityCheckOnInsert": {
            "choices": [True, False],
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        # The actual maximum of `replBatchLimitOperations` is 1000 * 1000 but this range doesn't work
        # for WINDOWS DEBUG, so that maximum is multiplied by 0.2, which is still a lot more than the
        # default value of 5000. The reason why the full range [1, 1000*1000] doesn't work on WINDOWS
        # DEBUG seems to be because it would wait for the batch to fill up to the batch limit
        # operations, but when that number is too high it would just time out before reaching the
        # batch limit operations.
        "replBatchLimitOperations": {
            "min": 1,
            "max": 0.2 * 1000 * 1000,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "replBatchLimitBytes": {
            "min": 16 * 1024 * 1024,
            "max": 100 * 1024 * 1024,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
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
        # Default value 1000; many tests don't insert enough measurements to rollover due to count, so we enable a larger range for this parameter.
        "timeseriesBucketMaxCount": {
            "min": 20,
            "max": 2000,
            "fuzz_at": ["startup"],
        },
        # Default value 10; maximum value is the lowest possible value of timeseriesBucketMaxCount.
        "timeseriesBucketMinCount": {"min": 1, "max": 20, "fuzz_at": ["startup"]},
        # Default value 128000 (125KB); many tests don't insert enough measurements to rollover due to size, so we enable a larger range for this parameter.
        "timeseriesBucketMaxSize": {
            "min": 5120,  # 5KB
            "max": 256000,  # 250KB
            "fuzz_at": ["startup"],
        },
        # Default value 5120; maximum value is the lowest possible value of timeseriesBucketMaxSize.
        "timeseriesBucketMinSize": {
            "min": 3072,  # 3KB
            "max": 5120,  # 5KB
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        # Default value 3; Enables more bucket re-opening by increasing the number of buckets that we can expire when performing idle bucket expiry.
        # Increasing bucket expiry was the most helpful way to increase re-opening because these buckets are still eligible for archived-based reopening, without side effects from doing more hard closes.
        # We also extended the lower side of the range so we can test the theoretical minimum (2) that enables the system to make progress.
        "timeseriesIdleBucketExpiryMaxCountPerAttempt": {
            "min": 2,
            "max": 32,
            "fuzz_at": ["startup"],
        },
        # Default value 32; doesn't contribute to increasing re-opening when being fuzzed with other parameters.
        # Having a lower value can increase rollover due to size because we have lower byte uncompressed size measurements having their uncompressed size being counted as the size in the bucket,
        # but fuzzing timeseriesLargeMeasurementThreshold at a lower range led to less re-opening when fuzzing with other parameters.
        "timeseriesLargeMeasurementThreshold": {
            "min": 28,
            "max": 36,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        # Default value 16; can help us identify flaky tests that rely on having/not having WriteConflicts during bucket re-opening.
        "timeseriesMaxRetriesForWriteConflictsOnReopening": {
            "min": 1,
            "max": 32,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        # Default value 104857600 (100 MB); Enables more bucket re-opening by decreasing the side bucket catalog memory threshold so we can more aggressively expire buckets.
        # Increasing bucket expiry was the most helpful way to increase re-opening because these buckets are still eligible for archived-based reopening, without side effects from doing more hard closes.
        "timeseriesSideBucketCatalogMemoryUsageThreshold": {
            "min": 200,
            "max": 500,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
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
        "wiredTigerCursorCacheSize": {"min": -100, "max": 0, "fuzz_at": ["startup"]},
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
            "min": 1,
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
        # Disable the periodic thread to abort multi-document transactions when under cache
        # pressure. As the storage engine parameters are fuzzed at runtime, it can make the thread
        # fire unpredictably, resulting in test failures that expect transactions to succeed.
        "cachePressureQueryPeriodMilliseconds": {"default": 0, "fuzz_at": ["startup"]},
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
        # TODO(SERVER-98867): re-enable config fuzzer signal testing.
        "failpoint.enableSignalTesting": {
            "choices": [{"mode": "off"}],
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
        "rollbackRemoteOplogQueryBatchSize": {
            "min": 1500,
            "max": 2500,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "writePeriodicNoops": {
            "choices": [True, False],
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "changeSyncSourceThresholdMillis": {
            "min": 0,
            "max": 20,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "maxNumSyncSourceChangesPerHour": {
            "min": 1,
            "max": 10,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "dbCheckMaxTotalIndexKeysPerSnapshot": {
            "min": 1,
            "max": 2000,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "writeConflictRetryLimit": {
            "min": 1,
            "max": 20000,
            "period": 5,
            "fuzz_at": ["startup", "runtime"],
        },
        "internalQueryStatsRateLimit": {
            "min": -1,
            "max": 1,
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
        "internalQueryStatsSampleRate": {
            "choices": [0, 0.0001, 0.1, 1.0],
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
        "internalQueryStatsCacheSize": {
            "choices": ["0.00001MB", "0MB", "1MB", "10MB"],
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
        # If we are enabling the above query stats flags in a test, we should also enable this one to catch any errors on collected queries.
        "internalQueryStatsErrorsAreCommandFatal": {
            "default": True,
            "fuzz_at": ["startup"],
        },
        "internalQueryPercentileExprSelectToSortThreshold": {
            "min": 0,
            "max": 30,
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
        # TODO(SERVER-98867): re-enable config fuzzer signal testing.
        "failpoint.enableSignalTesting": {
            "choices": [{"mode": "off"}],
            "fuzz_at": ["startup"],
        },
        "ingressConnectionEstablishmentRateLimiterEnabled": {
            "choices": [True, False],
            "period": 60,
            "fuzz_at": ["startup", "runtime"],
        },
        "ingressConnectionEstablishmentRatePerSec": {
            "min": 20,
            "max": 100_000,
            "period": 60,
            "fuzz_at": ["startup", "runtime"],
        },
        "ingressConnectionEstablishmentBurstCapacitySecs": {
            "min": 0.1,
            "max": 100_000,
            "isUniform": True,
            "period": 60,
            "fuzz_at": ["startup", "runtime"],
        },
        "ingressConnectionEstablishmentMaxQueueDepth": {
            "min": 100,
            "max": 100_000,
            "period": 60,
            "fuzz_at": ["startup", "runtime"],
        },
        "internalQueryStatsRateLimit": {
            "min": -1,
            "max": 1,
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
        "internalQueryStatsSampleRate": {
            "min": 0,
            "max": 1,
            "choices": [0, 0.0001, 0.1, 1.0],
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
        "internalQueryStatsCacheSize": {
            "choices": ["0.00001MB", "0MB", "1MB", "10MB"],
            "period": 10,
            "fuzz_at": ["startup", "runtime"],
        },
        # If we are enabling the above query stats flags in a test, we should also enable this one to catch any errors on collected queries.
        "internalQueryStatsErrorsAreCommandFatal": {
            "default": True,
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
