"""Generator functions for all parameters that we fuzz when invoked with --fuzzMongodConfigs."""

import random
from buildscripts.resmokelib import utils


def generate_eviction_configs(rng):
    """Generate random configurations for wiredTigerEngineConfigString parameter."""
    eviction_checkpoint_target = rng.randint(1, 99)
    eviction_target = rng.randint(50, 95)
    eviction_trigger = rng.randint(eviction_target + 1, 99)

    # Fuzz eviction_dirty_target and trigger both as relative and absolute values
    target_bytes_min = 10 * 1024 * 1024  # 10MB
    target_bytes_max = 256 * 1024 * 1024  # 256MB # 1GB default cache size on Evergreen
    eviction_dirty_target = rng.choice(
        [rng.randint(5, 50), rng.randint(target_bytes_min, target_bytes_max)])
    trigger_max = 75 if eviction_dirty_target <= 50 else target_bytes_max
    eviction_dirty_trigger = rng.randint(eviction_dirty_target, trigger_max)

    close_idle_time_secs = rng.randint(1, 100)
    close_handle_minimum = rng.randint(0, 1000)
    close_scan_interval = rng.randint(1, 100)

    return "eviction_checkpoint_target={0},eviction_dirty_target={1},eviction_dirty_trigger={2},"\
           "eviction_target={3},eviction_trigger={4},file_manager=(close_handle_minimum={5},"\
           "close_idle_time={6},close_scan_interval={7})".format(eviction_checkpoint_target,
                                                                 eviction_dirty_target,
                                                                 eviction_dirty_trigger,
                                                                 eviction_target,
                                                                 eviction_trigger,
                                                                 close_handle_minimum,
                                                                 close_idle_time_secs,
                                                                 close_scan_interval)


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


def generate_independent_parameters(rng):
    """Return a dictionary with values for each independent parameter."""
    ret = {}
    ret["wiredTigerCursorCacheSize"] = rng.randint(-100, 100)
    ret["wiredTigerSessionCloseIdleTimeSecs"] = rng.randint(0, 300)
    ret["wiredTigerConcurrentWriteTransactions"] = rng.randint(16, 256)
    ret["wiredTigerConcurrentReadTransactions"] = rng.randint(16, 256)
    if rng.choice(3 * [True] + [False]):
        # The old retryable writes format is used by other variants. Weight towards turning on the
        # new retryable writes format on in this one.
        ret["storeFindAndModifyImagesInSideCollection"] = True

    return ret


def fuzz_set_parameters(seed, user_provided_params):
    """Randomly generate mongod configurations and wiredTigerConnectionString."""
    rng = random.Random(seed)

    ret = {}
    params = [generate_flow_control_parameters(rng), generate_independent_parameters(rng)]
    for dct in params:
        for key, value in dct.items():
            ret[key] = value

    for key, value in utils.load_yaml(user_provided_params).items():
        ret[key] = value

    return utils.dump_yaml(ret), generate_eviction_configs(rng)
