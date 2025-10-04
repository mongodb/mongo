"""Minimum and maximum declarations for all WiredTiger parameters that are supported by the config fuzzer."""

"""
For context and maintenance, see:
https://github.com/10gen/mongo/blob/master/buildscripts/resmokelib/generate_fuzz_config/README.md#adding-new-wiredtiger-parameters
"""

target_bytes_min = 50 * 1024 * 1024  # 50MB # 5% of 1GB default cache size on Evergreen
target_bytes_max = 256 * 1024 * 1024  # 256MB # 25.6% of 1GB default cache size on Evergreen
# We want eviction_dirty_trigger and eviction_updates_trigger >= 64MB. See SERVER-96683.
min_trigger_bytes = 64 * 1024 * 1024  # 64MB # 6.4% of 1GB default cache size on Evergreen

config_fuzzer_params = {
    # WiredTiger's debug_mode offers some settings to change internal behavior that could help
    # find bugs. Settings to fuzz:
    # eviction - Turns aggressive eviction on/off
    # realloc_exact - Finds more memory bugs by allocating the memory for the exact size asked
    # rollback_error - Forces WiredTiger to return a rollback error every Nth call
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
        # 1% of 1GB default cache size on Evergreen, 99% of 1GB default cache size on Evergreen
        "eviction_checkpoint_target": {"min": 10 * 1024 * 1024, "max": 990 * 1024 * 1024},
        # 50% of 1GB default cache size on Evergreen, 95% of 1GB default cache size on Evergreen
        "eviction_target": {"min": 500 * 1024 * 1024, "max": 950 * 1024 * 1024},
        "eviction_trigger": {
            "lower_bound": 1,
            # 99% of 1GB default cache size on Evergreen
            "upper_bound": 990 * 1024 * 1024,
            "min": "(eviction_target + 1)",
            "max": 990 * 1024 * 1024,
        },
        "eviction_dirty_target": {
            "min": target_bytes_min,
            "max": target_bytes_max,
        },
        "eviction_dirty_trigger": {
            "min": "max(eviction_dirty_target + 1, min_trigger_bytes)",
            "max": "trigger_max",
        },
        # eviction_updates_target/trigger are relative to previously fuzzed values.
        "eviction_updates_target": {
            "min": "updates_target_min",
            "max": "eviction_dirty_target - 1",
        },
        "eviction_updates_trigger": {
            "min": "max(eviction_updates_target + 1, min_trigger_bytes)",
            "max": "eviction_dirty_trigger - 1",
        },
        "trigger_max": {"default": target_bytes_max},
        # We fuzz eviction_updates_target and eviction_updates_trigger using updates_target_min. These are
        # by default half the values of the corresponding eviction dirty target and trigger. They need to stay
        # less than the dirty equivalents. The default updates target is 2.5% of the cache, so let's start fuzzing
        # from 2%.
        "updates_target_min": {"default": 20 * 1024 * 1024},
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
