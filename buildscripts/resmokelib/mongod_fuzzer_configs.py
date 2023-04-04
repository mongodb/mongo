"""Generator functions for all parameters that we fuzz when invoked with --fuzzMongodConfigs."""

import random
from buildscripts.resmokelib import utils


def generate_eviction_configs(rng, mode):
    """Generate random configurations for wiredTigerEngineConfigString parameter."""
    eviction_checkpoint_target = rng.randint(1, 99)
    eviction_target = rng.randint(50, 95)
    eviction_trigger = rng.randint(eviction_target + 1, 99)

    # Fuzz eviction_dirty_target and trigger both as relative and absolute values
    target_bytes_min = 50 * 1024 * 1024  # 50MB # 5% of 1GB default cache size on Evergreen
    target_bytes_max = 256 * 1024 * 1024  # 256MB # 1GB default cache size on Evergreen
    eviction_dirty_target = rng.choice(
        [rng.randint(5, 50), rng.randint(target_bytes_min, target_bytes_max)])
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
    close_idle_time_secs = rng.randint(1, 100)
    close_handle_minimum = rng.randint(0, 1000)
    close_scan_interval = rng.randint(1, 100)

    # The debug_mode for WiredTiger offers some settings to change internal behavior that could help
    # find bugs. Settings to fuzz:
    # eviction - Turns aggressive eviction on/off
    # realloc_exact - Finds more memory bugs by allocating the memory for the exact size asked
    # rollback_error - Forces WiredTiger to return a rollback error every Nth call
    # slow_checkpoint - Adds internal delays in processing internal leaf pages during a checkpoint
    dbg_eviction = rng.choice(['true', 'false'])
    dbg_realloc_exact = rng.choice(['true', 'false'])
    # Rollback every Nth transaction. The values have been tuned after looking at how many
    # WiredTiger transactions happen per second for the config-fuzzed jstests.
    # The setting is trigerring bugs, disabled until they get resolved.
    # dbg_rollback_error = rng.choice([0, rng.randint(250, 1500)])
    dbg_rollback_error = 0
    dbg_slow_checkpoint = 'false' if mode != 'stress' else rng.choice(['true', 'false'])

    return "debug_mode=(eviction={0},realloc_exact={1},rollback_error={2}, slow_checkpoint={3}),"\
           "eviction_checkpoint_target={4},eviction_dirty_target={5},eviction_dirty_trigger={6},"\
           "eviction_target={7},eviction_trigger={8},eviction_updates_target={9},"\
           "eviction_updates_trigger={10},file_manager=(close_handle_minimum={11},"\
           "close_idle_time={12},close_scan_interval={13})".format(dbg_eviction,
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
                                                                 close_scan_interval)


def generate_table_configs(rng):
    """Generate random configurations for WiredTiger tables."""

    internal_page_max = rng.choice([4, 8, 12, 1024, 10 * 1024]) * 1024
    leaf_page_max = rng.choice([4, 8, 12, 1024, 10 * 1024]) * 1024
    leaf_value_max = rng.choice([1, 32, 128, 256]) * 1024 * 1024

    memory_page_max_lower_bound = leaf_page_max
    # Assume WT cache size of 1GB as most MDB tests specify this as the cache size.
    memory_page_max_upper_bound = round(
        (rng.randint(256, 1024) * 1024 * 1024) / 10)  # cache_size / 10
    memory_page_max = rng.randint(memory_page_max_lower_bound, memory_page_max_upper_bound)

    split_pct = rng.choice([50, 60, 75, 100])
    prefix_compression = rng.choice(["true", "false"])
    block_compressor = rng.choice(["none", "snappy", "zlib", "zstd"])

    return "block_compressor={0},internal_page_max={1},leaf_page_max={2},leaf_value_max={3},"\
           "memory_page_max={4},prefix_compression={5},split_pct={6}".format(block_compressor,
                                                                             internal_page_max,
                                                                             leaf_page_max,
                                                                             leaf_value_max,
                                                                             memory_page_max,
                                                                             prefix_compression,
                                                                             split_pct)


def generate_flow_control_parameters(rng):
    """Generate parameters related to flow control and returns a dictionary."""
    configs = {}
    configs["enableFlowControl"] = rng.choice([True, False])
    if not configs["enableFlowControl"]:
        return configs

    configs["flowControlTargetLagSeconds"] = rng.randint(1, 1000)
    configs["flowControlThresholdLagPercentage"] = rng.random()
    configs["flowControlMaxSamples"] = rng.randint(1, 1000 * 1000)
    configs["flowControlSamplePeriod"] = rng.randint(1, 1000 * 1000)
    configs["flowControlMinTicketsPerSecond"] = rng.randint(1, 10 * 1000)

    return configs


def generate_independent_parameters(rng, mode):
    """Return a dictionary with values for each independent parameter."""
    ret = {}
    ret["wiredTigerCursorCacheSize"] = rng.randint(-100, 100)
    ret["wiredTigerSessionCloseIdleTimeSecs"] = rng.randint(0, 300)
    ret["storageEngineConcurrencyAdjustmentAlgorithm"] = ""
    ret["wiredTigerConcurrentWriteTransactions"] = rng.randint(5, 32)
    ret["wiredTigerConcurrentReadTransactions"] = rng.randint(5, 32)
    ret["wiredTigerStressConfig"] = False if mode != 'stress' else rng.choice([True, False])
    if rng.choice(3 * [True] + [False]):
        # The old retryable writes format is used by other variants. Weight towards turning on the
        # new retryable writes format on in this one.
        ret["storeFindAndModifyImagesInSideCollection"] = True
    ret["syncdelay"] = rng.choice([60, rng.randint(15, 180)])
    ret["minSnapshotHistoryWindowInSeconds"] = rng.choice([300, rng.randint(5, 600)])
    # TODO (SERVER-75632): Uncomment this to enable passthrough testing.
    # ret["lockCodeSegmentsInMemory"] = rng.choice([True, False])

    return ret


def fuzz_set_parameters(mode, seed, user_provided_params):
    """Randomly generate mongod configurations and wiredTigerConnectionString."""
    rng = random.Random(seed)

    ret = {}
    params = [generate_flow_control_parameters(rng), generate_independent_parameters(rng, mode)]
    for dct in params:
        for key, value in dct.items():
            ret[key] = value

    for key, value in utils.load_yaml(user_provided_params).items():
        ret[key] = value

    return utils.dump_yaml(ret), generate_eviction_configs(rng, mode), generate_table_configs(rng), \
        generate_table_configs(rng)
