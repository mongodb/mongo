"""Minimum and maximum declarations for all WiredTiger parameters that are supported by the config fuzzer."""

target_bytes_min = 50 * 1024 * 1024  # 50MB # 5% of 1GB default cache size on Evergreen
target_bytes_max = 256 * 1024 * 1024  # 256MB # 1GB default cache size on Evergreen

config_fuzzer_params = {
    "wt": {
        "eviction_checkpoint_target": {"min": 1, "max": 99},
        "eviction_target": {"min": 50, "max": 95},
        "eviction_trigger": {"min": 1, "max": 99},
        "eviction_dirty_target_1": {"min": 5, "max": 50},
        "eviction_dirty_target_2": {"min": target_bytes_min, "max": target_bytes_max},
        "close_idle_time_secs": {"min": 1, "max": 100},
        "close_handle_minimum": {"min": 0, "max": 1000},
        "close_scan_interval": {"min": 1, "max": 100},
        "dbg_eviction": {"choices": ["true", "false"]},
        "dbg_realloc_exact": {"choices": ["true", "false"]},
        "dbg_slow_checkpoint": {"choices": ["true", "false"]},
    },
    "wt_table": {
        # These three parameters get multipled by additional values after the choice of the number is made.
        "internal_page_max": {
            "choices": [4, 8, 12, 1024, 10 * 1024],
            "min": 4 * 1024,
            "max": 10 * 1024 * 1024,
        },
        "leaf_page_max": {
            "choices": [4, 8, 12, 1024, 10 * 1024],
            "min": 4 * 1024,
            "max": 10 * 1024 * 1024,
        },
        "leaf_value_max": {
            "choices": [1, 32, 128, 256],
            "min": 1 * 1024 * 1024,
            "max": 256 * 1024 * 1024,
        },
        "split_pct": {"choices": [50, 60, 75, 100], "min": 50, "max": 100},
        "prefix_compression": {"choices": ["true", "false"]},
        "block_compressor": {"choices": ["none", "snappy", "zlib", "zstd"]},
        "memory_page_max_upper_bound": {"min": 256, "max": 1024},
    },
}
