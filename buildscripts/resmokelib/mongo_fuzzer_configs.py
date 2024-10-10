"""Generator functions for all parameters that we fuzz when invoked with --fuzzMongodConfigs."""

import json
import os
import random
import stat

from buildscripts.resmokelib import config, utils


def generate_normal_wt_parameters(rng, value):
    """Returns the value assigned the WiredTiger parameters (both eviction or table) based on the fields of the parameters in the config_fuzzer_wt_limits.py."""
    if "choices" in value:
        ret = rng.choice(value["choices"])
        if "multiplier" in value:
            ret *= value["multiplier"]
    elif "min" in value and "max" in value:
        ret = rng.randint(value["min"], value["max"])
    return ret


def generate_special_eviction_configs(rng, ret, fuzzer_stress_mode, params):
    """Returns the value assigned the WiredTiger eviction parameters based on the fields of the parameters in config_fuzzer_wt_limits.py for special parameters (parameters with different assignment behaviors)."""

    # eviction_trigger is relative to eviction_target, so you have to leave them excluded to ensure
    # eviction_trigger is fuzzed first.
    ret["eviction_target"] = rng.randint(
        params["eviction_target"]["min"], params["eviction_target"]["max"]
    )
    ret["eviction_trigger"] = rng.randint(
        ret["eviction_target"] + params["eviction_trigger"]["lower_bound"],
        params["eviction_trigger"]["upper_bound"],
    )

    # Fuzz eviction_dirty_target and trigger both as relative and absolute values.
    ret["eviction_dirty_target"] = rng.choice(
        [
            rng.randint(
                params["eviction_dirty_target_1"]["lower_bound"],
                params["eviction_dirty_target_1"]["upper_bound"],
            ),
            rng.randint(
                params["eviction_dirty_target_2"]["lower_bound"],
                params["eviction_dirty_target_2"]["upper_bound"],
            ),
        ]
    )
    ret["trigger_max"] = (
        params["trigger_max"]["min"]
        if ret["eviction_dirty_target"] <= 50
        else params["trigger_max"]["max"]
    )
    ret["eviction_dirty_trigger"] = rng.randint(
        ret["eviction_dirty_target"] + 1, ret["trigger_max"]
    )

    assert ret["eviction_dirty_trigger"] > ret["eviction_dirty_target"]
    assert ret["eviction_dirty_trigger"] <= ret["trigger_max"]

    # Fuzz eviction_updates_target and eviction_updates_trigger. These are by default half the
    # values of the corresponding eviction dirty target and trigger. They need to stay less than the
    # dirty equivalents. The default updates target is 2.5% of the cache, so let's start fuzzing
    # from 2%.
    ret["updates_target_min"] = (
        params["updates_target_min"]["min"]
        if ret["eviction_dirty_target"] <= 100
        else params["updates_target_min"]["max"]
    )  # 2% of 1GB cache
    ret["eviction_updates_target"] = rng.randint(
        ret["updates_target_min"], ret["eviction_dirty_target"] - 1
    )
    ret["eviction_updates_trigger"] = rng.randint(
        ret["eviction_updates_target"] + 1, ret["eviction_dirty_trigger"] - 1
    )

    # dbg_rollback_error rolls back every Nth transaction.
    # The values have been tuned after looking at how many WiredTiger transactions happen per second for the config-fuzzed jstests.
    # The setting is triggering bugs, disabled until they get resolved.
    ret["dbg_rollback_error"] = 0
    # choices = params["dbg_rollback_error"]["choices"]
    # choices.append(rng.randint(params["dbg_rollback_error"]["lower_bound"], params["dbg_rollback_error"]["upper_bound"]))
    # ret["dbg_rollback_error"] = rng.choice(choices)

    ret["dbg_slow_checkpoint"] = (
        "false"
        if fuzzer_stress_mode != "stress"
        else rng.choice(params["dbg_slow_checkpoint"]["choices"])
    )
    return ret


def generate_eviction_configs(rng, fuzzer_stress_mode):
    """Returns a string with random configurations for wiredTigerEngineConfigString parameter."""
    from buildscripts.resmokelib.config_fuzzer_wt_limits import config_fuzzer_params

    params = config_fuzzer_params["wt"]

    ret = {}
    excluded_normal_params = [
        "dbg_rollback_error",
        "dbg_slow_checkpoint",
        "eviction_dirty_target",
        "eviction_dirty_target_1",
        "eviction_dirty_target_2",
        "eviction_dirty_trigger",
        "eviction_target",
        "eviction_trigger",
        "eviction_updates_target",
        "eviction_updates_trigger",
        "trigger_max",
        "updates_target_min",
    ]

    ret = generate_special_eviction_configs(rng, ret, fuzzer_stress_mode, params)
    ret.update(
        {
            key: generate_normal_wt_parameters(rng, value)
            for key, value in params.items()
            if key not in excluded_normal_params
        }
    )

    return (
        "debug_mode=(eviction={0},realloc_exact={1},rollback_error={2}, slow_checkpoint={3}),"
        "eviction_checkpoint_target={4},eviction_dirty_target={5},eviction_dirty_trigger={6},"
        "eviction_target={7},eviction_trigger={8},eviction_updates_target={9},"
        "eviction_updates_trigger={10},file_manager=(close_handle_minimum={11},"
        "close_idle_time={12},close_scan_interval={13})".format(
            ret["dbg_eviction"],
            ret["dbg_realloc_exact"],
            ret["dbg_rollback_error"],
            ret["dbg_slow_checkpoint"],
            ret["eviction_checkpoint_target"],
            ret["eviction_dirty_target"],
            ret["eviction_dirty_trigger"],
            ret["eviction_target"],
            ret["eviction_trigger"],
            ret["eviction_updates_target"],
            ret["eviction_updates_trigger"],
            ret["close_handle_minimum"],
            ret["close_idle_time_secs"],
            ret["close_scan_interval"],
        )
    )


def generate_special_table_configs(rng, ret, params):
    """Returns the value assigned the WiredTiger table parameters based on the fields of the parameters in config_fuzzer_wt_limits.py for special parameters (parameters with different assignment behaviors)."""

    ret["memory_page_max_lower_bound"] = ret["leaf_page_max"]
    # Assume WT cache size of 1GB as most MDB tests specify this as the cache size.
    ret["memory_page_max_upper_bound"] = round(
        (
            rng.randint(
                params["memory_page_max_upper_bound"]["lower_bound"],
                params["memory_page_max_upper_bound"]["upper_bound"],
            )
            * params["memory_page_max_upper_bound"]["multiplier"]
        )
        / 10
    )  # cache_size / 10
    ret["memory_page_max"] = rng.randint(
        ret["memory_page_max_lower_bound"], ret["memory_page_max_upper_bound"]
    )
    return ret


def generate_table_configs(rng):
    """Returns a string with random configurations for WiredTiger tables."""
    from buildscripts.resmokelib.config_fuzzer_wt_limits import config_fuzzer_params

    params = config_fuzzer_params["wt_table"]

    ret = {}
    # excluded_normal_params are a list of params that we want to exclude from the for-loop because they have some different assignment behavior
    # e.g. depending on other parameters' values, having rounding, having a different distribution.
    excluded_normal_params = [
        "memory_page_max_lower_bound",
        "memory_page_max_upper_bound",
        "memory_page_max",
    ]

    ret.update(
        {
            key: generate_normal_wt_parameters(rng, value)
            for key, value in params.items()
            if key not in excluded_normal_params
        }
    )
    ret = generate_special_table_configs(rng, ret, params)

    return (
        "block_compressor={0},internal_page_max={1},leaf_page_max={2},leaf_value_max={3},"
        "memory_page_max={4},prefix_compression={5},split_pct={6}".format(
            ret["block_compressor"],
            ret["internal_page_max"],
            ret["leaf_page_max"],
            ret["leaf_value_max"],
            ret["memory_page_max"],
            ret["prefix_compression"],
            ret["split_pct"],
        )
    )


def generate_encryption_config(rng: random.Random):
    ret = {}
    # encryption requires the wiredtiger storage engine.
    # encryption also required an enterprise binary.
    if (
        config.STORAGE_ENGINE != "wiredTiger"
        or config.ENABLE_ENTERPRISE_TESTS != "on"
        or config.DISABLE_ENCRYPTION_FUZZING
    ):
        return ret
    chance_to_encrypt = 0.33
    if rng.random() < chance_to_encrypt:
        ret["enableEncryption"] = ""
        encryption_key_file = "src/mongo/db/modules/enterprise/jstests/encryptdb/libs/ekf2"

        # Antithesis runs mongo processes in a docker container separate from the resmoke process.
        # It cannot use the absolute path from the machine that resmoke running on.
        # Other applications, such as Jepsen, can be run in a different directory than the root
        # of the mongo directory so we use the absolute path.
        if not config.NOOP_MONGO_D_S_PROCESSES:
            encryption_key_file = os.path.abspath(encryption_key_file)

        # Set file permissions to avoid "too open" error.
        # MongoDB requires keyfiles to have restricted permissions.
        # Since git doesn't preserve file permissions across clones,
        # we need to explicitly set them to a state Mongo accepts.
        os.chmod(encryption_key_file, stat.S_IRUSR | stat.S_IWUSR)
        ret["encryptionKeyFile"] = encryption_key_file

        chance_to_use_gcm = 0.50
        if rng.random() < chance_to_use_gcm:
            ret["encryptionCipherMode"] = "AES256-GCM"

    return ret


def generate_normal_mongo_parameters(rng, value):
    """Returns the value assigned the mongod or mongos parameter based on the fields of the parameters in the config_fuzzer_limits.py."""

    if "isUniform" in value:
        ret = rng.uniform(value["min"], value["max"])
    elif "isRandomizedChoice" in value:
        choices = value["choices"]
        choices.append(rng.randint(value["lower_bound"], value["upper_bound"]))
        ret = rng.choice(choices)
    elif "choices" in value:
        ret = rng.choice(value["choices"])
    elif "min" in value and "max" in value:
        ret = rng.randint(value["min"], value["max"])
        if "multiplier" in value:
            ret *= value["multiplier"]
    elif "default" in value:
        ret = value["default"]
    return ret


def generate_special_mongod_parameters(rng, ret, fuzzer_stress_mode, params):
    """Returns the value assigned the mongod parameter based on the fields of the parameters in config_fuzzer_limits.py for special parameters (parameters with different assignment behaviors)."""
    ret["storageEngineConcurrencyAdjustmentAlgorithm"] = rng.choices(
        params["storageEngineConcurrencyAdjustmentAlgorithm"]["choices"], weights=[10, 1]
    )[0]

    # We assign the wiredTigerConcurrent(Read/Write)Transactions only if the storageEngineConcurrencyAdjustmentAlgorithm is fixedConcurrentTransactions.
    # Otherwise, we will disable throughput probing.
    if ret["storageEngineConcurrencyAdjustmentAlgorithm"] == "fixedConcurrentTransactions":
        ret["wiredTigerConcurrentReadTransactions"] = rng.randint(
            params["wiredTigerConcurrentReadTransactions"]["min"],
            params["wiredTigerConcurrentReadTransactions"]["max"],
        )
        ret["wiredTigerConcurrentWriteTransactions"] = rng.randint(
            params["wiredTigerConcurrentWriteTransactions"]["min"],
            params["wiredTigerConcurrentWriteTransactions"]["max"],
        )
    # We assign the throughputProbing* parameters only if the storageEngineConcurrencyAdjustmentAlgorithm is throughputProbing.
    else:
        # throughputProbingConcurrencyMovingAverageWeight is the only parameter that uses rng.random().
        ret["throughputProbingConcurrencyMovingAverageWeight"] = 1 - rng.random()

        # We assign throughputProbingInitialConcurrency first because throughputProbingMinConcurrency and throughputProbingMaxConcurrency depend on it.
        ret["throughputProbingInitialConcurrency"] = rng.randint(
            params["throughputProbingInitialConcurrency"]["min"],
            params["throughputProbingInitialConcurrency"]["max"],
        )
        ret["throughputProbingMinConcurrency"] = rng.randint(
            params["throughputProbingMinConcurrency"]["min"],
            ret["throughputProbingInitialConcurrency"],
        )
        ret["throughputProbingMaxConcurrency"] = rng.randint(
            ret["throughputProbingInitialConcurrency"],
            params["throughputProbingMaxConcurrency"]["max"],
        )
        ret["throughputProbingReadWriteRatio"] = rng.uniform(
            params["throughputProbingReadWriteRatio"]["min"],
            params["throughputProbingReadWriteRatio"]["max"],
        )
        ret["throughputProbingStepMultiple"] = rng.uniform(
            params["throughputProbingStepMultiple"]["min"],
            params["throughputProbingStepMultiple"]["max"],
        )

    # mirrorReads sets a nested samplingRate field.
    ret["mirrorReads"] = {"samplingRate": rng.choice(params["mirrorReads"]["choices"])}

    # Deal with other special cases of parameters (having to add other sources of randomization, depending on another variable, etc.).
    ret["internalQueryExecYieldIterations"] = rng.choices(
        [
            1,
            rng.randint(
                params["internalQueryExecYieldIterations"]["lower_bound"],
                params["internalQueryExecYieldIterations"]["upper_bound"],
            ),
        ],
        weights=[1, 10],
    )[0]
    ret["maxNumberOfTransactionOperationsInSingleOplogEntry"] = rng.randint(1, 10) * rng.choice(
        params["maxNumberOfTransactionOperationsInSingleOplogEntry"]["choices"]
    )

    ret["wiredTigerStressConfig"] = (
        False
        if fuzzer_stress_mode != "stress"
        else rng.choice(params["wiredTigerStressConfig"]["choices"])
    )
    ret["disableLogicalSessionCacheRefresh"] = rng.choice(
        params["disableLogicalSessionCacheRefresh"]["choices"]
    )
    if not ret["disableLogicalSessionCacheRefresh"]:
        ret["logicalSessionRefreshMillis"] = rng.choice(
            params["logicalSessionRefreshMillis"]["choices"]
        )
    if rng.random() >= 0.1:
        ret["failpoint.hangAfterPreCommittingCatalogUpdates"] = {"mode": "off"}
        ret["failpoint.hangBeforePublishingCatalogUpdates"] = {"mode": "off"}
    else:
        waitMillisMax = params["failpoint.hangAfterPreCommittingCatalogUpdates"][
            "pauseEntireCommitMillis"
        ]["max"]
        waitMillisMin = params["failpoint.hangAfterPreCommittingCatalogUpdates"][
            "pauseEntireCommitMillis"
        ]["min"]
        ret["failpoint.hangAfterPreCommittingCatalogUpdates"] = {
            "mode": {"activationProbability": random.uniform(0, 0.5)},
            "data": {"pauseEntireCommitMillis": rng.randint(waitMillisMin, waitMillisMax)},
        }
        waitMillisMax = params["failpoint.hangBeforePublishingCatalogUpdates"][
            "pauseEntireCommitMillis"
        ]["max"]
        waitMillisMin = params["failpoint.hangBeforePublishingCatalogUpdates"][
            "pauseEntireCommitMillis"
        ]["min"]
        ret["failpoint.hangBeforePublishingCatalogUpdates"] = {
            "mode": {"activationProbability": random.uniform(0, 0.5)},
            "data": {"pauseEntireCommitMillis": rng.randint(waitMillisMin, waitMillisMax)},
        }
    return ret


def generate_flow_control_parameters(rng, ret, flow_control_params, params):
    """Returns an updated dictionary which assigns fuzzed flow control parameters for mongod."""

    # Assigning flow control parameters.
    ret["enableFlowControl"] = rng.choice(params["enableFlowControl"]["choices"])
    if ret["enableFlowControl"]:
        for name in flow_control_params:
            if "isUniform" in params[name]:
                ret[name] = rng.uniform(params[name]["min"], params[name]["max"])
            else:
                ret[name] = rng.randint(params[name]["min"], params[name]["max"])
        ret["flowControlThresholdLagPercentage"] = rng.random()
    return ret


def generate_mongod_parameters(rng, fuzzer_stress_mode):
    """Return a dictionary with values for each mongod parameter."""
    from buildscripts.resmokelib.config_fuzzer_limits import config_fuzzer_params

    # Get only the mongod parameters that have "startup" in the "fuzz_at" param value.
    params = {
        param: val
        for param, val in config_fuzzer_params["mongod"].items()
        if "startup" in val.get("fuzz_at", [])
    }

    # Parameter sets with different behaviors.
    flow_control_params = [
        "flowControlTicketAdderConstant",
        "flowControlDecayConstant",
        "flowControlFudgeFactor",
        "flowControlMaxSamples",
        "flowControlMinTicketsPerSecond",
        "flowControlTicketMultiplierConstant",
        "flowControlSamplePeriod",
        "flowControlTargetLagSeconds",
        "flowControlThresholdLagPercentage",
        "flowControlWarnThresholdSeconds",
    ]

    # excluded_normal_params are params that we want to exclude from the for-loop because they have some different assignment behavior
    # e.g. depending on other parameters' values, having rounding, having a different distribution.
    excluded_normal_params = [
        "disableLogicalSessionCacheRefresh",
        "internalQueryExecYieldIterations",
        "logicalSessionRefreshMillis",
        "maxNumberOfTransactionOperationsInSingleOplogEntry",
        "mirrorReads",
        "storageEngineConcurrencyAdjustmentAlgorithm",
        "throughputProbingConcurrencyMovingAverageWeight",
        "throughputProbingInitialConcurrency",
        "throughputProbingMinConcurrency",
        "throughputProbingMaxConcurrency",
        "throughputProbingReadWriteRatio",
        "throughputProbingStepMultiple",
        "wiredTigerConcurrentReadTransactions",
        "wiredTigerConcurrentWriteTransactions",
        "wiredTigerStressConfig",
        "failpoint.hangAfterPreCommittingCatalogUpdates",
        "failpoint.hangBeforePublishingCatalogUpdates",
    ]
    # TODO (SERVER-75632): Remove/comment out the below line to enable passthrough testing.
    excluded_normal_params.append("lockCodeSegmentsInMemory")

    ret = {}
    # Range through all other parameters and assign the parameters based on the keys that are available or the parameter set lists defined above.
    ret.update(
        {
            key: generate_normal_mongo_parameters(rng, value)
            for key, value in params.items()
            if key not in excluded_normal_params and key not in flow_control_params
        }
    )

    ret = generate_special_mongod_parameters(rng, ret, fuzzer_stress_mode, params)
    ret = generate_flow_control_parameters(rng, ret, flow_control_params, params)
    return ret


def generate_mongos_parameters(rng):
    """Return a dictionary with values for each mongos parameter."""
    from buildscripts.resmokelib.config_fuzzer_limits import config_fuzzer_params

    # Get only the mongos parameters that have "startup" in the "fuzz_at" param value.
    params = {
        param: val
        for param, val in config_fuzzer_params["mongos"].items()
        if "startup" in val.get("fuzz_at", [])
    }

    return {key: generate_normal_mongo_parameters(rng, value) for key, value in params.items()}


def fuzz_mongod_set_parameters(fuzzer_stress_mode, seed, user_provided_params):
    """Randomly generate mongod configurations and wiredTigerConnectionString."""
    rng = random.Random(seed)

    ret = {}
    mongod_params = generate_mongod_parameters(rng, fuzzer_stress_mode)

    for key, value in mongod_params.items():
        ret[key] = value

    for key, value in utils.load_yaml(user_provided_params).items():
        ret[key] = value

    for key, value in ret.items():
        # We may at times contain a dictionary for the parameter value, in order to pass them via
        # setParameter we must dump them as a JSON.
        if isinstance(value, dict):
            value = json.dumps(value)
        ret[key] = value

    return (
        utils.dump_yaml(ret),
        generate_eviction_configs(rng, fuzzer_stress_mode),
        generate_table_configs(rng),
        generate_table_configs(rng),
        generate_encryption_config(rng),
    )


def fuzz_mongos_set_parameters(seed, user_provided_params):
    """Randomly generate mongos configurations."""
    rng = random.Random(seed)

    ret = {}
    params = generate_mongos_parameters(rng)
    for key, value in params.items():
        ret[key] = value

    for key, value in utils.load_yaml(user_provided_params).items():
        ret[key] = value

    for key, value in ret.items():
        # We may at times contain a dictionary for the parameter value, in order to pass them
        # via setParameter we must dump them as a JSON.
        if isinstance(value, dict):
            value = json.dumps(value)
        ret[key] = value

    return utils.dump_yaml(ret)
