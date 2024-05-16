"""Generator functions for all parameters that we fuzz when invoked with --fuzzMongodConfigs."""

import random
from buildscripts.resmokelib import utils


def generate_eviction_configs(rng, fuzzer_stress_mode):
    """Generate random configurations for wiredTigerEngineConfigString parameter."""
    from buildscripts.resmokelib.config_fuzzer_wt_limits import (
        config_fuzzer_params,
        target_bytes_max,
    )

    params = config_fuzzer_params["wt"]
    eviction_checkpoint_target = rng.randint(
        params["eviction_checkpoint_target"]["min"], params["eviction_checkpoint_target"]["max"]
    )
    eviction_target = rng.randint(
        params["eviction_target"]["min"], params["eviction_target"]["max"]
    )
    eviction_trigger = rng.randint(
        eviction_target + params["eviction_trigger"]["min"], params["eviction_trigger"]["max"]
    )

    # Fuzz eviction_dirty_target and trigger both as relative and absolute values
    eviction_dirty_target = rng.choice(
        [
            rng.randint(
                params["eviction_dirty_target_1"]["min"], params["eviction_dirty_target_1"]["max"]
            ),
            rng.randint(
                params["eviction_dirty_target_2"]["min"], params["eviction_dirty_target_2"]["max"]
            ),
        ]
    )
    trigger_max = 75 if eviction_dirty_target <= 50 else target_bytes_max
    eviction_dirty_trigger = rng.randint(eviction_dirty_target + 1, trigger_max)

    assert eviction_dirty_trigger > eviction_dirty_target
    assert eviction_dirty_trigger <= trigger_max

    # Fuzz eviction_updates_target and eviction_updates_trigger. These are by default half the
    # values of the corresponding eviction dirty target and trigger. They need to stay less than the
    # dirty equivalents. The default updates target is 2.5% of the cache, so let's start fuzzing
    # from 2%.
    updates_target_min = 2 if eviction_dirty_target <= 100 else 20 * 1024 * 1024  # 2% of 1GB cache
    eviction_updates_target = rng.randint(updates_target_min, eviction_dirty_target - 1)
    eviction_updates_trigger = rng.randint(eviction_updates_target + 1, eviction_dirty_trigger - 1)

    # Fuzz File manager settings
    close_idle_time_secs = rng.randint(
        params["close_idle_time_secs"]["min"], params["close_idle_time_secs"]["max"]
    )
    close_handle_minimum = rng.randint(
        params["close_handle_minimum"]["min"], params["close_handle_minimum"]["max"]
    )
    close_scan_interval = rng.randint(
        params["close_scan_interval"]["min"], params["close_scan_interval"]["max"]
    )

    # WiredTiger's debug_mode offers some settings to change internal behavior that could help
    # find bugs. Settings to fuzz:
    # eviction - Turns aggressive eviction on/off
    # realloc_exact - Finds more memory bugs by allocating the memory for the exact size asked
    # rollback_error - Forces WiredTiger to return a rollback error every Nth call
    # slow_checkpoint - Adds internal delays in processing internal leaf pages during a checkpoint
    dbg_eviction = rng.choice(params["dbg_eviction"]["choices"])
    dbg_realloc_exact = rng.choice(params["dbg_realloc_exact"]["choices"])
    # Rollback every Nth transaction. The values have been tuned after looking at how many
    # WiredTiger transactions happen per second for the config-fuzzed jstests.
    # The setting is trigerring bugs, disabled until they get resolved.
    # dbg_rollback_error = rng.choice([0, rng.randint(250, 1500)])
    dbg_rollback_error = 0
    dbg_slow_checkpoint = (
        "false"
        if fuzzer_stress_mode != "stress"
        else rng.choice(params["dbg_slow_checkpoint"]["choices"])
    )

    return (
        "debug_mode=(eviction={0},realloc_exact={1},rollback_error={2}, slow_checkpoint={3}),"
        "eviction_checkpoint_target={4},eviction_dirty_target={5},eviction_dirty_trigger={6},"
        "eviction_target={7},eviction_trigger={8},eviction_updates_target={9},"
        "eviction_updates_trigger={10},file_manager=(close_handle_minimum={11},"
        "close_idle_time={12},close_scan_interval={13})".format(
            dbg_eviction,
            dbg_realloc_exact,
            dbg_rollback_error,
            dbg_slow_checkpoint,
            eviction_checkpoint_target,
            eviction_dirty_target,
            eviction_dirty_trigger,
            eviction_target,
            eviction_trigger,
            eviction_updates_target,
            eviction_updates_trigger,
            close_handle_minimum,
            close_idle_time_secs,
            close_scan_interval,
        )
    )


def generate_table_configs(rng):
    """Generate random configurations for WiredTiger tables."""
    from buildscripts.resmokelib.config_fuzzer_wt_limits import config_fuzzer_params

    params = config_fuzzer_params["wt_table"]
    internal_page_max = rng.choice(params["internal_page_max"]["choices"]) * 1024
    leaf_page_max = rng.choice(params["leaf_page_max"]["choices"]) * 1024
    leaf_value_max = rng.choice(params["leaf_value_max"]["choices"]) * 1024 * 1024

    memory_page_max_lower_bound = leaf_page_max
    # Assume WT cache size of 1GB as most MDB tests specify this as the cache size.
    memory_page_max_upper_bound = round(
        (
            rng.randint(
                params["memory_page_max_upper_bound"]["min"],
                params["memory_page_max_upper_bound"]["max"],
            )
            * 1024
            * 1024
        )
        / 10
    )  # cache_size / 10
    memory_page_max = rng.randint(memory_page_max_lower_bound, memory_page_max_upper_bound)

    split_pct = rng.choice(params["split_pct"]["choices"])
    prefix_compression = rng.choice(params["prefix_compression"]["choices"])
    block_compressor = rng.choice(params["block_compressor"]["choices"])

    return (
        "block_compressor={0},internal_page_max={1},leaf_page_max={2},leaf_value_max={3},"
        "memory_page_max={4},prefix_compression={5},split_pct={6}".format(
            block_compressor,
            internal_page_max,
            leaf_page_max,
            leaf_value_max,
            memory_page_max,
            prefix_compression,
            split_pct,
        )
    )


def generate_mongod_parameters(rng, fuzzer_stress_mode):
    """Return a dictionary with values for each mongod parameter."""
    from buildscripts.resmokelib.config_fuzzer_limits import config_fuzzer_params

    params = config_fuzzer_params["mongod"]
    ret = {}
    ret["analyzeShardKeySplitPointExpirationSecs"] = rng.randint(
        params["analyzeShardKeySplitPointExpirationSecs"]["min"],
        params["analyzeShardKeySplitPointExpirationSecs"]["max"],
    )
    ret["chunkMigrationConcurrency"] = rng.choice(params["chunkMigrationConcurrency"]["choices"])
    ret["disableLogicalSessionCacheRefresh"] = rng.choice(
        params["disableLogicalSessionCacheRefresh"]["choices"]
    )
    ret["enableAutoCompaction"] = rng.choice(params["enableAutoCompaction"]["choices"])
    ret["initialServiceExecutorUseDedicatedThread"] = rng.choice(
        params["initialServiceExecutorUseDedicatedThread"]["choices"]
    )
    # TODO (SERVER-75632): Uncomment this to enable passthrough testing.
    # ret["lockCodeSegmentsInMemory"] = rng.choice([True, False])
    if not ret["disableLogicalSessionCacheRefresh"]:
        ret["logicalSessionRefreshMillis"] = rng.choice(
            params["logicalSessionRefreshMillis"]["choices"]
        )
    ret["maxNumberOfTransactionOperationsInSingleOplogEntry"] = rng.randint(1, 10) * rng.choice(
        params["maxNumberOfTransactionOperationsInSingleOplogEntry"]["choices"]
    )
    ret["wiredTigerCursorCacheSize"] = rng.randint(
        params["wiredTigerCursorCacheSize"]["min"], params["wiredTigerCursorCacheSize"]["max"]
    )
    ret["wiredTigerSessionCloseIdleTimeSecs"] = rng.randint(
        params["wiredTigerSessionCloseIdleTimeSecs"]["min"],
        params["wiredTigerSessionCloseIdleTimeSecs"]["max"],
    )
    ret["storageEngineConcurrencyAdjustmentIntervalMillis"] = rng.randint(
        params["storageEngineConcurrencyAdjustmentIntervalMillis"]["min"],
        params["storageEngineConcurrencyAdjustmentIntervalMillis"]["max"],
    )
    ret["throughputProbingStepMultiple"] = rng.uniform(
        params["throughputProbingStepMultiple"]["min"],
        params["throughputProbingStepMultiple"]["max"],
    )
    ret["throughputProbingInitialConcurrency"] = rng.randint(
        params["throughputProbingInitialConcurrency"]["min"],
        params["throughputProbingInitialConcurrency"]["max"],
    )
    ret["throughputProbingMinConcurrency"] = rng.randint(
        params["throughputProbingMinConcurrency"]["min"], ret["throughputProbingInitialConcurrency"]
    )
    ret["throughputProbingMaxConcurrency"] = rng.randint(
        ret["throughputProbingInitialConcurrency"], params["throughputProbingMaxConcurrency"]["max"]
    )
    ret["storageEngineConcurrencyAdjustmentAlgorithm"] = rng.choices(
        params["storageEngineConcurrencyAdjustmentAlgorithm"]["choices"], weights=[10, 1]
    )[0]
    ret["throughputProbingReadWriteRatio"] = rng.uniform(
        params["throughputProbingReadWriteRatio"]["min"],
        params["throughputProbingReadWriteRatio"]["max"],
    )
    ret["minSnapshotHistoryWindowInSeconds"] = rng.choice([300, rng.randint(30, 600)])
    ret["mirrorReads"] = {"samplingRate": rng.random()}
    ret["syncdelay"] = rng.choice([60, rng.randint(15, 180)])
    ret["throughputProbingConcurrencyMovingAverageWeight"] = 1 - rng.random()
    ret["wiredTigerConcurrentWriteTransactions"] = rng.randint(
        params["wiredTigerConcurrentWriteTransactions"]["min"],
        params["wiredTigerConcurrentWriteTransactions"]["max"],
    )
    ret["wiredTigerConcurrentReadTransactions"] = rng.randint(
        params["wiredTigerConcurrentReadTransactions"]["min"],
        params["wiredTigerConcurrentReadTransactions"]["max"],
    )
    ret["wiredTigerStressConfig"] = (
        False
        if fuzzer_stress_mode != "stress"
        else rng.choice(params["wiredTigerStressConfig"]["choices"])
    )
    ret["wiredTigerSizeStorerPeriodicSyncHits"] = rng.randint(
        params["wiredTigerSizeStorerPeriodicSyncHits"]["min"],
        params["wiredTigerSizeStorerPeriodicSyncHits"]["max"],
    )
    ret["wiredTigerSizeStorerPeriodicSyncPeriodMillis"] = rng.randint(
        params["wiredTigerSizeStorerPeriodicSyncPeriodMillis"]["min"],
        params["wiredTigerSizeStorerPeriodicSyncPeriodMillis"]["max"],
    )

    ret["oplogFetcherUsesExhaust"] = rng.choice(params["oplogFetcherUsesExhaust"]["choices"])
    ret["replWriterThreadCount"] = rng.randint(
        params["replWriterThreadCount"]["min"], params["replWriterThreadCount"]["max"]
    )

    ret["replBatchLimitOperations"] = rng.randint(
        params["replBatchLimitOperations"]["min"], params["replBatchLimitOperations"]["max"]
    )
    ret["replBatchLimitBytes"] = rng.randint(
        params["replBatchLimitBytes"]["min"], params["replBatchLimitBytes"]["max"]
    )
    ret["initialSyncSourceReadPreference"] = rng.choice(
        params["initialSyncSourceReadPreference"]["choices"]
    )
    ret["initialSyncMethod"] = rng.choice(params["initialSyncMethod"]["choices"])

    # Query parameters
    ret["queryAnalysisWriterMaxMemoryUsageBytes"] = rng.randint(
        params["queryAnalysisWriterMaxMemoryUsageBytes"]["min"],
        params["queryAnalysisWriterMaxMemoryUsageBytes"]["max"],
    )
    ret["internalQueryExecYieldIterations"] = rng.choices(
        [
            1,
            rng.randint(
                params["internalQueryExecYieldIterations"]["min"],
                params["internalQueryExecYieldIterations"]["max"],
            ),
        ],
        weights=[1, 10],
    )[0]
    ret["internalQueryExecYieldPeriodMS"] = rng.randint(
        params["internalQueryExecYieldPeriodMS"]["min"],
        params["internalQueryExecYieldPeriodMS"]["max"],
    )

    # Flow control parameters
    ret["enableFlowControl"] = rng.choice(params["enableFlowControl"]["choices"])
    if ret["enableFlowControl"]:
        ret["flowControlThresholdLagPercentage"] = rng.random()
        param_names = [
            "flowControlTargetLagSeconds",
            "flowControlMaxSamples",
            "flowControlSamplePeriod",
            "flowControlMinTicketsPerSecond",
        ]
        for name in param_names:
            ret[name] = rng.randint(params[name]["min"], params[name]["max"])

    # We need a higher timeout to account for test slowness
    ret["receiveChunkWaitForRangeDeleterTimeoutMS"] = 300000
    ret["defaultConfigCommandTimeoutMS"] = 90000
    return ret


def generate_mongos_parameters(rng, fuzzer_stress_mode):
    """Return a dictionary with values for each mongos parameter."""
    from buildscripts.resmokelib.config_fuzzer_limits import config_fuzzer_params

    params = config_fuzzer_params["mongos"]
    ret = {}
    ret["initialServiceExecutorUseDedicatedThread"] = rng.choice(
        params["initialServiceExecutorUseDedicatedThread"]["choices"]
    )
    ret["opportunisticSecondaryTargeting"] = rng.choice(
        params["opportunisticSecondaryTargeting"]["choices"]
    )

    # We need a higher timeout to account for test slowness
    ret["defaultConfigCommandTimeoutMS"] = 90000
    return ret


def fuzz_mongod_set_parameters(fuzzer_stress_mode, seed, user_provided_params):
    """Randomly generate mongod configurations and wiredTigerConnectionString."""
    rng = random.Random(seed)

    ret = {}
    mongod_params = generate_mongod_parameters(rng, fuzzer_stress_mode)

    for key, value in mongod_params.items():
        ret[key] = value

    for key, value in utils.load_yaml(user_provided_params).items():
        ret[key] = value

    return (
        utils.dump_yaml(ret),
        generate_eviction_configs(rng, fuzzer_stress_mode),
        generate_table_configs(rng),
        generate_table_configs(rng),
    )


def fuzz_mongos_set_parameters(fuzzer_stress_mode, seed, user_provided_params):
    """Randomly generate mongos configurations."""
    rng = random.Random(seed)

    ret = {}
    params = generate_mongos_parameters(rng, fuzzer_stress_mode)
    for key, value in params.items():
        ret[key] = value

    for key, value in utils.load_yaml(user_provided_params).items():
        ret[key] = value

    return utils.dump_yaml(ret)
