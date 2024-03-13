/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

struct __wt_config {
    WT_SESSION_IMPL *session;
    const char *orig;
    const char *end;
    const char *cur;

    int depth, top;
    const int8_t *go;
};

/*
 * We have jump tables that for each ASCII character, show the offset in a lookup table that keys
 * starting with that character start at. All keys are 7 bit ASCII.
 */
#define WT_CONFIG_JUMP_TABLE_SIZE 128

/*
 * The expected types of a configuration value.
 */
#define WT_CONFIG_COMPILED_TYPE_INT 0
#define WT_CONFIG_COMPILED_TYPE_BOOLEAN 1
#define WT_CONFIG_COMPILED_TYPE_FORMAT 2
#define WT_CONFIG_COMPILED_TYPE_STRING 3
#define WT_CONFIG_COMPILED_TYPE_CATEGORY 4
#define WT_CONFIG_COMPILED_TYPE_LIST 5

struct __wt_config_check {
    const char *name;
    const char *type;
    int (*checkf)(WT_SESSION_IMPL *, WT_CONFIG_ITEM *);
    const char *checks;
    const WT_CONFIG_CHECK *subconfigs;
    u_int subconfigs_entries;
    const uint8_t *subconfigs_jump;
    u_int compiled_type;
    u_int key_id;
    int64_t min_value;
    int64_t max_value;
    const char **choices;
};

#define WT_CONFIG_REF(session, n) (S2C(session)->config_entries[WT_CONFIG_ENTRY_##n])
struct __wt_config_entry {
    const char *method; /* method name */

#define WT_CONFIG_BASE(session, n) (WT_CONFIG_REF(session, n)->base)
    const char *base; /* configuration base */

    const WT_CONFIG_CHECK *checks; /* check array */
    u_int checks_entries;
    const uint8_t *checks_jump;
    u_int method_id;
    size_t conf_total_size; /* total size of the structures needed for precompiling */
    u_int conf_count;       /* number of WT_CONF structures needed for precompiling */
    u_int conf_value_count; /* number of WT_CONF_VALUE structures needed for precompiling */
    bool compilable;
};

struct __wt_config_parser_impl {
    WT_CONFIG_PARSER iface;

    WT_SESSION_IMPL *session;
    WT_CONFIG config;
    WT_CONFIG_ITEM config_item;
};

/*
 * C++ treats nested structure definitions differently to C, as such we need to use scope resolution
 * to fully define the type.
 */
#ifdef __cplusplus
#define WT_CONFIG_ITEM_STATIC_INIT(n) \
    static const WT_CONFIG_ITEM n = {"", 0, 0, WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM}
#else
#define WT_CONFIG_ITEM_STATIC_INIT(n) static const WT_CONFIG_ITEM n = {"", 0, 0, WT_CONFIG_ITEM_NUM}
#endif

/*
 * If double quotes surround the string, then expand the string to include them. This is always
 * called in the context of keys or values returned by the configuration parser. The character after
 * the string must be at a valid memory address, and checking just that one is sufficient. If it is
 * a double quote, then the character before must be as well, by the rules of the tokenizer.
 */
#define WT_CONFIG_PRESERVE_QUOTES(session, item)        \
    do {                                                \
        if ((item)->str[(item)->len] == '"') {          \
            WT_ASSERT(session, (item)->str[-1] == '"'); \
            (item)->str -= 1;                           \
            (item)->len += 2;                           \
        }                                               \
    } while (0)

#define WT_CONFIG_UNSET (-1)
/*
 * DO NOT EDIT: automatically built by dist/api_config.py.
 * configuration section: BEGIN
 */
#define WT_CONFIG_ENTRY_WT_CONNECTION_add_collator 0
#define WT_CONFIG_ENTRY_WT_CONNECTION_add_compressor 1
#define WT_CONFIG_ENTRY_WT_CONNECTION_add_data_source 2
#define WT_CONFIG_ENTRY_WT_CONNECTION_add_encryptor 3
#define WT_CONFIG_ENTRY_WT_CONNECTION_add_extractor 4
#define WT_CONFIG_ENTRY_WT_CONNECTION_add_storage_source 5
#define WT_CONFIG_ENTRY_WT_CONNECTION_close 6
#define WT_CONFIG_ENTRY_WT_CONNECTION_debug_info 7
#define WT_CONFIG_ENTRY_WT_CONNECTION_load_extension 8
#define WT_CONFIG_ENTRY_WT_CONNECTION_open_session 9
#define WT_CONFIG_ENTRY_WT_CONNECTION_query_timestamp 10
#define WT_CONFIG_ENTRY_WT_CONNECTION_reconfigure 11
#define WT_CONFIG_ENTRY_WT_CONNECTION_rollback_to_stable 12
#define WT_CONFIG_ENTRY_WT_CONNECTION_set_file_system 13
#define WT_CONFIG_ENTRY_WT_CONNECTION_set_timestamp 14
#define WT_CONFIG_ENTRY_WT_CURSOR_bound 15
#define WT_CONFIG_ENTRY_WT_CURSOR_close 16
#define WT_CONFIG_ENTRY_WT_CURSOR_reconfigure 17
#define WT_CONFIG_ENTRY_WT_SESSION_alter 18
#define WT_CONFIG_ENTRY_WT_SESSION_begin_transaction 19
#define WT_CONFIG_ENTRY_WT_SESSION_checkpoint 20
#define WT_CONFIG_ENTRY_WT_SESSION_close 21
#define WT_CONFIG_ENTRY_WT_SESSION_commit_transaction 22
#define WT_CONFIG_ENTRY_WT_SESSION_compact 23
#define WT_CONFIG_ENTRY_WT_SESSION_create 24
#define WT_CONFIG_ENTRY_WT_SESSION_drop 25
#define WT_CONFIG_ENTRY_WT_SESSION_join 26
#define WT_CONFIG_ENTRY_WT_SESSION_log_flush 27
#define WT_CONFIG_ENTRY_WT_SESSION_log_printf 28
#define WT_CONFIG_ENTRY_WT_SESSION_open_cursor 29
#define WT_CONFIG_ENTRY_WT_SESSION_prepare_transaction 30
#define WT_CONFIG_ENTRY_WT_SESSION_query_timestamp 31
#define WT_CONFIG_ENTRY_WT_SESSION_reconfigure 32
#define WT_CONFIG_ENTRY_WT_SESSION_reset 33
#define WT_CONFIG_ENTRY_WT_SESSION_reset_snapshot 34
#define WT_CONFIG_ENTRY_WT_SESSION_rollback_transaction 35
#define WT_CONFIG_ENTRY_WT_SESSION_salvage 36
#define WT_CONFIG_ENTRY_WT_SESSION_strerror 37
#define WT_CONFIG_ENTRY_WT_SESSION_timestamp_transaction 38
#define WT_CONFIG_ENTRY_WT_SESSION_timestamp_transaction_uint 39
#define WT_CONFIG_ENTRY_WT_SESSION_truncate 40
#define WT_CONFIG_ENTRY_WT_SESSION_upgrade 41
#define WT_CONFIG_ENTRY_WT_SESSION_verify 42
#define WT_CONFIG_ENTRY_colgroup_meta 43
#define WT_CONFIG_ENTRY_file_config 44
#define WT_CONFIG_ENTRY_file_meta 45
#define WT_CONFIG_ENTRY_index_meta 46
#define WT_CONFIG_ENTRY_lsm_meta 47
#define WT_CONFIG_ENTRY_object_meta 48
#define WT_CONFIG_ENTRY_table_meta 49
#define WT_CONFIG_ENTRY_tier_meta 50
#define WT_CONFIG_ENTRY_tiered_meta 51
#define WT_CONFIG_ENTRY_wiredtiger_open 52
#define WT_CONFIG_ENTRY_wiredtiger_open_all 53
#define WT_CONFIG_ENTRY_wiredtiger_open_basecfg 54
#define WT_CONFIG_ENTRY_wiredtiger_open_usercfg 55

extern const char __WT_CONFIG_CHOICE_NULL[]; /* not set in configuration */
extern const char __WT_CONFIG_CHOICE_DRAM[];
extern const char __WT_CONFIG_CHOICE_FILE[];
extern const char __WT_CONFIG_CHOICE_aggressive_stash_free[];
extern const char __WT_CONFIG_CHOICE_aggressive_sweep[];
extern const char __WT_CONFIG_CHOICE_all[];
extern const char __WT_CONFIG_CHOICE_all_durable[];
extern const char __WT_CONFIG_CHOICE_always[];
extern const char __WT_CONFIG_CHOICE_and[];
extern const char __WT_CONFIG_CHOICE_api[];
extern const char __WT_CONFIG_CHOICE_backup[];
extern const char __WT_CONFIG_CHOICE_backup_rename[];
extern const char __WT_CONFIG_CHOICE_best[];
extern const char __WT_CONFIG_CHOICE_block[];
extern const char __WT_CONFIG_CHOICE_block_cache[];
extern const char __WT_CONFIG_CHOICE_bloom[];
extern const char __WT_CONFIG_CHOICE_btree[];
extern const char __WT_CONFIG_CHOICE_cache_walk[];
extern const char __WT_CONFIG_CHOICE_checkpoint[];
extern const char __WT_CONFIG_CHOICE_checkpoint_cleanup[];
extern const char __WT_CONFIG_CHOICE_checkpoint_evict_page[];
extern const char __WT_CONFIG_CHOICE_checkpoint_handle[];
extern const char __WT_CONFIG_CHOICE_checkpoint_progress[];
extern const char __WT_CONFIG_CHOICE_checkpoint_slow[];
extern const char __WT_CONFIG_CHOICE_checkpoint_stop[];
extern const char __WT_CONFIG_CHOICE_checkpoint_validate[];
extern const char __WT_CONFIG_CHOICE_chunkcache[];
extern const char __WT_CONFIG_CHOICE_clear[];
extern const char __WT_CONFIG_CHOICE_commit[];
extern const char __WT_CONFIG_CHOICE_compact[];
extern const char __WT_CONFIG_CHOICE_compact_progress[];
extern const char __WT_CONFIG_CHOICE_compact_slow[];
extern const char __WT_CONFIG_CHOICE_configuration[];
extern const char __WT_CONFIG_CHOICE_cursor_check[];
extern const char __WT_CONFIG_CHOICE_data[];
extern const char __WT_CONFIG_CHOICE_default[];
extern const char __WT_CONFIG_CHOICE_disk_validate[];
extern const char __WT_CONFIG_CHOICE_dsync[];
extern const char __WT_CONFIG_CHOICE_eq[];
extern const char __WT_CONFIG_CHOICE_error[];
extern const char __WT_CONFIG_CHOICE_error_returns[];
extern const char __WT_CONFIG_CHOICE_evict[];
extern const char __WT_CONFIG_CHOICE_evict_reposition[];
extern const char __WT_CONFIG_CHOICE_evict_stuck[];
extern const char __WT_CONFIG_CHOICE_eviction_check[];
extern const char __WT_CONFIG_CHOICE_evictserver[];
extern const char __WT_CONFIG_CHOICE_failpoint_eviction_split[];
extern const char __WT_CONFIG_CHOICE_failpoint_history_store_delete_key_from_ts[];
extern const char __WT_CONFIG_CHOICE_false[];
extern const char __WT_CONFIG_CHOICE_fast[];
extern const char __WT_CONFIG_CHOICE_fileops[];
extern const char __WT_CONFIG_CHOICE_first[];
extern const char __WT_CONFIG_CHOICE_first_commit[];
extern const char __WT_CONFIG_CHOICE_force[];
extern const char __WT_CONFIG_CHOICE_fsync[];
extern const char __WT_CONFIG_CHOICE_ge[];
extern const char __WT_CONFIG_CHOICE_generation[];
extern const char __WT_CONFIG_CHOICE_generation_check[];
extern const char __WT_CONFIG_CHOICE_gt[];
extern const char __WT_CONFIG_CHOICE_handleops[];
extern const char __WT_CONFIG_CHOICE_hex[];
extern const char __WT_CONFIG_CHOICE_history_store[];
extern const char __WT_CONFIG_CHOICE_history_store_activity[];
extern const char __WT_CONFIG_CHOICE_history_store_checkpoint_delay[];
extern const char __WT_CONFIG_CHOICE_history_store_search[];
extern const char __WT_CONFIG_CHOICE_history_store_sweep_race[];
extern const char __WT_CONFIG_CHOICE_hs_validate[];
extern const char __WT_CONFIG_CHOICE_json[];
extern const char __WT_CONFIG_CHOICE_key_consistent[];
extern const char __WT_CONFIG_CHOICE_key_out_of_order[];
extern const char __WT_CONFIG_CHOICE_last_checkpoint[];
extern const char __WT_CONFIG_CHOICE_le[];
extern const char __WT_CONFIG_CHOICE_log[];
extern const char __WT_CONFIG_CHOICE_log_validate[];
extern const char __WT_CONFIG_CHOICE_lower[];
extern const char __WT_CONFIG_CHOICE_lsm[];
extern const char __WT_CONFIG_CHOICE_lsm_manager[];
extern const char __WT_CONFIG_CHOICE_lt[];
extern const char __WT_CONFIG_CHOICE_message[];
extern const char __WT_CONFIG_CHOICE_metadata[];
extern const char __WT_CONFIG_CHOICE_mixed_mode[];
extern const char __WT_CONFIG_CHOICE_mutex[];
extern const char __WT_CONFIG_CHOICE_never[];
extern const char __WT_CONFIG_CHOICE_none[];
extern const char __WT_CONFIG_CHOICE_off[];
extern const char __WT_CONFIG_CHOICE_oldest[];
extern const char __WT_CONFIG_CHOICE_oldest_reader[];
extern const char __WT_CONFIG_CHOICE_oldest_timestamp[];
extern const char __WT_CONFIG_CHOICE_on[];
extern const char __WT_CONFIG_CHOICE_or[];
extern const char __WT_CONFIG_CHOICE_ordered[];
extern const char __WT_CONFIG_CHOICE_out_of_order[];
extern const char __WT_CONFIG_CHOICE_overflow[];
extern const char __WT_CONFIG_CHOICE_pinned[];
extern const char __WT_CONFIG_CHOICE_prefix_compare[];
extern const char __WT_CONFIG_CHOICE_prepare[];
extern const char __WT_CONFIG_CHOICE_prepare_checkpoint_delay[];
extern const char __WT_CONFIG_CHOICE_prepare_resolution_1[];
extern const char __WT_CONFIG_CHOICE_prepare_resolution_2[];
extern const char __WT_CONFIG_CHOICE_prepared[];
extern const char __WT_CONFIG_CHOICE_pretty[];
extern const char __WT_CONFIG_CHOICE_pretty_hex[];
extern const char __WT_CONFIG_CHOICE_print[];
extern const char __WT_CONFIG_CHOICE_random[];
extern const char __WT_CONFIG_CHOICE_read[];
extern const char __WT_CONFIG_CHOICE_read_committed[];
extern const char __WT_CONFIG_CHOICE_read_uncommitted[];
extern const char __WT_CONFIG_CHOICE_reclaim_space[];
extern const char __WT_CONFIG_CHOICE_reconcile[];
extern const char __WT_CONFIG_CHOICE_recovery[];
extern const char __WT_CONFIG_CHOICE_recovery_progress[];
extern const char __WT_CONFIG_CHOICE_rts[];
extern const char __WT_CONFIG_CHOICE_salvage[];
extern const char __WT_CONFIG_CHOICE_sequential[];
extern const char __WT_CONFIG_CHOICE_set[];
extern const char __WT_CONFIG_CHOICE_shared_cache[];
extern const char __WT_CONFIG_CHOICE_size[];
extern const char __WT_CONFIG_CHOICE_sleep_before_read_overflow_onpage[];
extern const char __WT_CONFIG_CHOICE_slow_operation[];
extern const char __WT_CONFIG_CHOICE_snapshot[];
extern const char __WT_CONFIG_CHOICE_split[];
extern const char __WT_CONFIG_CHOICE_split_1[];
extern const char __WT_CONFIG_CHOICE_split_2[];
extern const char __WT_CONFIG_CHOICE_split_3[];
extern const char __WT_CONFIG_CHOICE_split_4[];
extern const char __WT_CONFIG_CHOICE_split_5[];
extern const char __WT_CONFIG_CHOICE_split_6[];
extern const char __WT_CONFIG_CHOICE_split_7[];
extern const char __WT_CONFIG_CHOICE_split_8[];
extern const char __WT_CONFIG_CHOICE_stable[];
extern const char __WT_CONFIG_CHOICE_stable_timestamp[];
extern const char __WT_CONFIG_CHOICE_temporary[];
extern const char __WT_CONFIG_CHOICE_thread_group[];
extern const char __WT_CONFIG_CHOICE_tiered[];
extern const char __WT_CONFIG_CHOICE_tiered_flush_finish[];
extern const char __WT_CONFIG_CHOICE_timestamp[];
extern const char __WT_CONFIG_CHOICE_transaction[];
extern const char __WT_CONFIG_CHOICE_tree_walk[];
extern const char __WT_CONFIG_CHOICE_true[];
extern const char __WT_CONFIG_CHOICE_txn_visibility[];
extern const char __WT_CONFIG_CHOICE_uncompressed[];
extern const char __WT_CONFIG_CHOICE_unencrypted[];
extern const char __WT_CONFIG_CHOICE_upper[];
extern const char __WT_CONFIG_CHOICE_verify[];
extern const char __WT_CONFIG_CHOICE_version[];
extern const char __WT_CONFIG_CHOICE_write[];
extern const char __WT_CONFIG_CHOICE_write_timestamp[];
/*
 * configuration section: END
 * DO NOT EDIT: automatically built by dist/flags.py.
 */
