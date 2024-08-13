"""Minimum and maximum dictionary declarations for the different randomized parameters (mongod and mongos)."""

"""
How to add a fuzzed mongod/mongos parameter:

Below is a list of ways to fuzz configs which are supported without having to also change buildscripts/resmokelib/mongo_fuzzer_configs.py.
Please ensure that you add it correctly to the "mongod" or "mongos" subdictionary.
Let choices = [choice1, choice2, ..., choiceN] (an array of choices that the parameter can have as a value).
The parameters are added in order of priority chosen in the if-elif-else statement in generate_normal_mongo_parameters()
in buildscripts/resmokelib/mongo_fuzzer_configs.py.
So, if you added the fields "default", "min", and "max" for a "param", case 4 would get evaluated over case 5.

1. param = rng.uniform(lower_bound, upper_bound)
    Add:
    “param”: {“min”: lower_bound, “max”: upper_bound, "isUniform": True}

2. param = rng.randint([choices, rng.randint(lower_bound, upper_bound)])
    Add:
    "param": {"min": <min of lower_bound and choices>, "max": <max of upper_bound and choices>, "lower_bound": lower_bound, "upper_bound": upper_bound, "choices": [choice1, choice2, ..., choiceN], "isRandomizedChoice": true}

3. param = rng.choices(choices), where choices is an array
    Add:
    "param": {"choices": [choice1, choice2, ..., choiceN]}

4. param = rng.randint(lower_bound, upper_bound)
    Add:
    “param”: {“min”: lower_bound, “max”: upper_bound}

5. param = default
    Add:
    "param": {"default": default}

If you have a parameter that depends on another parameter being generated (see throughputProbingInitialConcurrency needing to be initialized before
throughputProbingMinConcurrency and throughputProbingMaxConcurrency as an example in buildscripts/resmokelib/mongo_fuzzer_configs.py) or behavior that
differs from the above cases, please do the following steps:
1. Add the parameter and the needed information about the parameters here (ensure to correctly add to the mongod or mongos sub-dictionary)

In buildscripts/resmokelib/mongo_fuzzer_configs.py:
2. Add the parameter to excluded_normal_parameters in the generate_mongod_parameters() or generate_mongos_parameters()
3. Add the parameter's special handling in generate_special_mongod_parameters() or generate_special_mongos_parameters()

If you add a flow control parameter, please add the the parameter's name to flow_control_params in generate_mongod_parameters.

If you would like the fuzzer to change the value of the parameter periodically at _runtime_, rather than just at startup, add your 
parameter to the runtime_parameter_fuzzer_params dictionary below. The format for describing how a value should be selected is the same
as what was described above; additionally, the dictionary for each parameter must contain a 'period' key that describes how often the 
parameter should be changed, in seconds. Every 'period' seconds, the fuzzer will select a new random value for the parameter and use the 
setParameter command to update the value of the parameter on every node in the cluster while the suite is running.
"""

config_fuzzer_params = {
    "mongod": {
        "analyzeShardKeySplitPointExpirationSecs": {"min": 1, "max": 300},
        "chunkMigrationConcurrency": {"choices": [1, 4, 16], "min": 1, "max": 16},
        "disableLogicalSessionCacheRefresh": {
            "choices": [True, False],
        },
        "enableAutoCompaction": {
            "choices": [True, False],
        },
        "initialServiceExecutorUseDedicatedThread": {
            "choices": [True, False],
        },
        "initialSyncMethod": {"choices": ["fileCopyBased", "logical"]},
        # For `initialSyncSourceReadPreference`, the option `secondary` is excluded from the fuzzer
        # because the generated mongod parameters are used for every node in the replica set, so the
        # secondaries in the replica set will not be able to find a valid sync source.
        "initialSyncSourceReadPreference": {
            "choices": ["nearest", "primary", "primaryPreferred", "secondaryPreferred"],
        },
        "internalQueryExecYieldIterations": {"min": 1, "max": 1000},
        "internalQueryExecYieldPeriodMS": {"min": 1, "max": 100},
        "internalQueryFindCommandBatchSize": {"min": 1, "max": 500},
        "lockCodeSegmentsInMemory": {"choices": [True, False]},
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
        "oplogFetcherUsesExhaust": {
            "choices": [True, False],
        },
        # The actual maximum of `replBatchLimitOperations` is 1000 * 1000 but this range doesn't work
        # for WINDOWS DEBUG, so that maximum is multiplied by 0.2, which is still a lot more than the
        # default value of 5000. The reason why the full range [1, 1000*1000] doesn't work on WINDOWS
        # DEBUG seems to be because it would wait for the batch to fill up to the batch limit
        # operations, but when that number is too high it would just time out before reaching the
        # batch limit operations.
        "replBatchLimitOperations": {"min": 1, "max": 0.2 * 1000 * 1000},
        "replBatchLimitBytes": {"min": 16 * 1024 * 1024, "max": 100 * 1024 * 1024},
        "replWriterThreadCount": {"min": 1, "max": 256},
        "storageEngineConcurrencyAdjustmentAlgorithm": {
            "choices": ["throughputProbing", "fixedConcurrentTransactions"],
        },
        "storageEngineConcurrencyAdjustmentIntervalMillis": {"min": 10, "max": 1000},
        "throughputProbingStepMultiple": {"min": 0.1, "max": 0.5, "isUniform": True},
        "throughputProbingInitialConcurrency": {"min": 4, "max": 128},
        "throughputProbingMinConcurrency": {"min": 4, "max": "throughputProbingInitialConcurrency"},
        "throughputProbingMaxConcurrency": {
            "min": "throughputProbingInitialConcurrency",
            "max": 128,
        },
        "throughputProbingReadWriteRatio": {"min": 0, "max": 1, "isUniform": True},
        "throughputProbingConcurrencyMovingAverageWeight": {"min": 0.0, "max": 1.0},
        "wiredTigerStressConfig": {
            "choices": [True, False],
        },
        "wiredTigerCursorCacheSize": {"min": -100, "max": 100},
        "wiredTigerSessionCloseIdleTimeSecs": {"min": 0, "max": 300},
        "wiredTigerConcurrentWriteTransactions": {"min": 5, "max": 32},
        "wiredTigerConcurrentReadTransactions": {"min": 5, "max": 32},
        "wiredTigerSizeStorerPeriodicSyncHits": {"min": 1, "max": 100000},
        "wiredTigerSizeStorerPeriodicSyncPeriodMillis": {"min": 1, "max": 60000},
        "queryAnalysisWriterMaxMemoryUsageBytes": {"min": 1024 * 1024, "max": 1024 * 1024 * 100},
        "mirrorReads": {"choices": [0, 0.25, 0.50, 0.75, 1.0]},
        # Flow control related parameters
        "enableFlowControl": {"choices": [True, False]},
        "flowControlMaxSamples": {"min": 1, "max": 1000 * 1000},
        "flowControlMinTicketsPerSecond": {"min": 1, "max": 10 * 1000},
        "flowControlSamplePeriod": {"min": 1, "max": 1000 * 1000},
        "flowControlTargetLagSeconds": {"min": 1, "max": 1000},
        "flowControlThresholdLagPercentage": {"min": 0.0, "max": 1.0},
        # We need a higher timeout to account for test slowness
        "receiveChunkWaitForRangeDeleterTimeoutMS": {"default": 300000},
        "defaultConfigCommandTimeoutMS": {"default": 90000},
        # These parameters have a min, max, and a choice with one value because we first find rng.randint(min, max)
        # and then add this value to the choices array and call rng.choices(choices).
        "minSnapshotHistoryWindowInSeconds": {
            "choices": [300],
            "lower_bound": 30,
            "upper_bound": 600,
            "min": 30,
            "max": 600,
            "isRandomizedChoice": True,
        },
        "syncdelay": {
            "choices": [60],
            "lower_bound": 15,
            "upper_bound": 180,
            "min": 15,
            "max": 180,
            "isRandomizedChoice": True,
        },
    },
    "mongos": {
        # We need a higher timeout to account for test slowness
        "defaultConfigCommandTimeoutMS": {"default": 90000},
        "initialServiceExecutorUseDedicatedThread": {"choices": [True, False]},
        "internalQueryFindCommandBatchSize": {"min": 1, "max": 500},
        "opportunisticSecondaryTargeting": {"choices": [True, False]},
    },
}

runtime_parameter_fuzzer_params = {
    "mongod": {
        "ingressAdmissionControllerTicketPoolSize": {
            "choices": [1_000, 10_000, 100_000, 1_000_000],
            "lower_bound": 1_000,
            "upper_bound": 1_000_000,
            "isRandomizedChoice": True,
            "period": 5,
        },
    },
    "mongos": {
        "userCacheInvalidationIntervalSecs": {"min": 1, "max": 86400, "period": 5},
    },
}
