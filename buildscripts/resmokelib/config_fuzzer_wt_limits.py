"""Minimum and maximum declarations for all WiredTiger parameters that are supported by the config fuzzer."""

"""
How to add a fuzzed WiredTiger parameters:

Below is a list of ways to fuzz configs which are supported without having to also change buildscripts/resmokelib/mongo_fuzzer_configs.py.
Please ensure that you add it correctly to the "wt" (eviction parameters) or "wt_table" subdictionary.
Let choices = [choice1, choice2, ..., choiceN] (an array of choices that the parameter can have as a value).
The parameters are added in order of priority chosen in the if-elif-else statement in generate_normal_wt_parameters() in
buildscripts/resmokelib/mongo_fuzzer_configs.py.

1. param = rng.choices(choices), where choices is an array
    Add:
    "param": {"choices": [choice1, choice2, ..., choiceN]}

    You can also add a "multiplier" key which multiplies the key by the multiplier value.
    param = rng.choice(choices) * multiplier
    Add:
    "param": {"choices": [choice1, choice2, ..., choiceN], "multiplier": multiplier}

2. param = rng.randint(min, max)
    Add:
    “param”: {“min”: min, “max”: max}

If you have a parameter that depends on another parameter being generated (see eviction_target needing to be initialized before
eviction_trigger as an example in buildscripts/resmokelib/mongo_fuzzer_configs.py) or behavior that differs from the above cases, 
please do the following step:
1. Add the parameter and the needed information about the parameters here (ensure to correctly add to the wt or wt_table sub-dictionary)

In buildscripts/resmokelib/mongo_fuzzer_configs.py:
2. Add the parameter to excluded_normal_parameters in the generate_eviction_configs() or generate_table_configs()
3. Add the parameter's special handling in generate_special_eviction_configs() or generate_special_table_configs()

Note: The main distinction between min/max vs. lower-bound/upper_bound is there is some transformation involving the lower and upper bounds,
while the min/max should be the true min/max of the parameters. You should also include the true min/max of the parameter so this can be logged.
If the min/max is not inclusive, this is added as a note above the parameter.
"""

target_bytes_min = 50 * 1024 * 1024  # 50MB # 5% of 1GB default cache size on Evergreen
target_bytes_max = 256 * 1024 * 1024  # 256MB # 1GB default cache size on Evergreen

config_fuzzer_params = {
    # WiredTiger's debug_mode offers some settings to change internal behavior that could help
    # find bugs. Settings to fuzz:
    # eviction - Turns aggressive eviction on/off
    # realloc_exact - Finds more memory bugs by allocating the memory for the exact size asked
    # rollback_error - Forces WiredTiger to return a rollback error every Nth call
    # slow_checkpoint - Adds internal delays in processing internal leaf pages during a checkpoint
    "wt": {
        # The following three parameters are for fuzzing file manager settings.
        "close_idle_time_secs": {"min": 1, "max": 100},
        "close_handle_minimum": {"min": 0, "max": 1000},
        "close_scan_interval": {"min": 1, "max": 100},
        "dbg_eviction": {"choices": ["true", "false"]},
        "dbg_realloc_exact": {"choices": ["true", "false"]},
        "dbg_rollback_error": {
            "min": 0,
            "max": 1500,
            "lower_bound": 250,
            "upper_bound": 1500,
            "choices": [0],
        },
        "dbg_slow_checkpoint": {"choices": ["true", "false"]},
        "eviction_checkpoint_target": {"min": 1, "max": 99},
        "eviction_target": {"min": 50, "max": 95},
        # eviction_trigger's true minimum value is (eviction_target + 1), but we have the min set to 1 because this is the value that we'll add to get the lower bound of the eviction_trigger.
        "eviction_trigger": {
            "lower_bound": 1,
            "upper_bound": 99,
            "min": "(eviction_target + 1)",
            "max": 99,
        },
        # eviction_dirty_target has two different lower-/upper-bounds because it is found by doing a choice between the two eviction_dirty_target_* ranges.
        "eviction_dirty_target_1": {"lower_bound": 5, "upper_bound": 50},
        "eviction_dirty_target_2": {
            "lower_bound": target_bytes_min,
            "upper_bound": target_bytes_max,
        },
        "eviction_dirty_trigger": {
            "min": "eviction_dirty_target + 1",
            "max": "trigger_max",
        },
        # eviction_updates_target/trigger are relative to previously fuzzed values.
        "eviction_updates_target": {
            "min": "updates_target_min",
            "max": "eviction_dirty_target - 1",
        },
        "eviction_updates_trigger": {
            "min": "eviction_updates_target + 1",
            "max": "eviction_dirty_trigger - 1",
        },
        "trigger_max": {"min": 75, "max": target_bytes_max},
        # updates_target_min depends on the value of eviction_dirty_target.
        "updates_target_min": {"min": 2, "max": 20 * 1024 * 1024},
    },
    "wt_table": {
        "block_compressor": {"choices": ["none", "snappy", "zlib", "zstd"]},
        # The following three parameters get multipled by their multiplier after the choice in choices is made.
        "internal_page_max": {
            "choices": [4, 8, 12, 1024, 10 * 1024],
            "min": 4 * 1024,
            "max": 10 * 1024 * 1024,
            "multiplier": 1024,
        },
        "leaf_page_max": {
            "choices": [4, 8, 12, 1024, 10 * 1024],
            "min": 4 * 1024,
            "max": 10 * 1024 * 1024,
            "multiplier": 1024,
        },
        "leaf_value_max": {
            "choices": [1, 32, 128, 256],
            "min": 1 * 1024 * 1024,
            "max": 256 * 1024 * 1024,
            "multiplier": 1024 * 1024,
        },
        "memory_page_max_upper_bound": {
            "min": (256 * 1024 * 1024) / 10,
            "max": (1024 * 1024 * 1024) / 10,
            "multiplier": 1024 * 1024,
            "lower_bound": 256,
            "upper_bound": 1024,
        },
        "prefix_compression": {"choices": ["true", "false"]},
        "split_pct": {"choices": [50, 60, 75, 100], "min": 50, "max": 100},
    },
}
# eviction_dirty_target is declared to store the true min/max of the eviction_dirty_target parameter, even though we use eviction_dirty_target_* above to generate the value.
config_fuzzer_params["eviction_dirty_target"] = {
    "min": min(
        config_fuzzer_params["wt"]["eviction_dirty_target_1"]["lower_bound"],
        config_fuzzer_params["wt"]["eviction_dirty_target_2"]["lower_bound"],
    ),
    "max": max(
        config_fuzzer_params["wt"]["eviction_dirty_target_1"]["upper_bound"],
        config_fuzzer_params["wt"]["eviction_dirty_target_2"]["upper_bound"],
    ),
}
