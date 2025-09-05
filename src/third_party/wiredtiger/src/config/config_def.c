/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"
const char __WT_CONFIG_CHOICE_NULL[] = ""; /* not set in configuration */

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_close[] = {
  {"final_flush", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 179,
    INT64_MIN, INT64_MAX, NULL},
  {"leak_memory", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 180,
    INT64_MIN, INT64_MAX, NULL},
  {"use_timestamp", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 178,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CONNECTION_close_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
  1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_debug_info[] = {
  {"backup", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 181, INT64_MIN,
    INT64_MAX, NULL},
  {"cache", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 182, INT64_MIN,
    INT64_MAX, NULL},
  {"cursors", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 183, INT64_MIN,
    INT64_MAX, NULL},
  {"handles", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 184, INT64_MIN,
    INT64_MAX, NULL},
  {"log", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 185, INT64_MIN,
    INT64_MAX, NULL},
  {"metadata", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 186,
    INT64_MIN, INT64_MAX, NULL},
  {"sessions", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 187,
    INT64_MIN, INT64_MAX, NULL},
  {"txn", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 188, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CONNECTION_debug_info_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 3, 3,
  3, 3, 4, 4, 4, 4, 5, 6, 6, 6, 6, 6, 6, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_load_extension[] = {
  {"config", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 292, INT64_MIN,
    INT64_MAX, NULL},
  {"early_load", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 293,
    INT64_MIN, INT64_MAX, NULL},
  {"entry", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 294, INT64_MIN,
    INT64_MAX, NULL},
  {"terminate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 295, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CONNECTION_load_extension_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
  1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_open_session_debug_subconfigs[] = {
  {"checkpoint_fail_before_turtle_update", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 297, INT64_MIN, INT64_MAX, NULL},
  {"release_evict_page", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 298,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_CONNECTION_open_session_debug_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
const char __WT_CONFIG_CHOICE_read_uncommitted[] = "read-uncommitted";
const char __WT_CONFIG_CHOICE_read_committed[] = "read-committed";
const char __WT_CONFIG_CHOICE_snapshot[] = "snapshot";

static const char *confchk_isolation_choices[] = {__WT_CONFIG_CHOICE_read_uncommitted,
  __WT_CONFIG_CHOICE_read_committed, __WT_CONFIG_CHOICE_snapshot, NULL};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_open_session_prefetch_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_CONNECTION_open_session_prefetch_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_open_session[] = {
  {"cache_cursors", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 296,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 203, 0,
    INT64_MAX, NULL},
  {"debug", "category", NULL, NULL, confchk_WT_CONNECTION_open_session_debug_subconfigs, 2,
    confchk_WT_CONNECTION_open_session_debug_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 125,
    INT64_MIN, INT64_MAX, NULL},
  {"ignore_cache_size", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 299,
    INT64_MIN, INT64_MAX, NULL},
  {"isolation", "string", NULL,
    "choices=[\"read-uncommitted\",\"read-committed\","
    "\"snapshot\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 164, INT64_MIN, INT64_MAX,
    confchk_isolation_choices},
  {"prefetch", "category", NULL, NULL, confchk_WT_CONNECTION_open_session_prefetch_subconfigs, 1,
    confchk_WT_CONNECTION_open_session_prefetch_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    300, INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CONNECTION_open_session_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3,
  3, 3, 3, 3, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};
const char __WT_CONFIG_CHOICE_all_durable[] = "all_durable";
const char __WT_CONFIG_CHOICE_backup_checkpoint[] = "backup_checkpoint";
const char __WT_CONFIG_CHOICE_last_checkpoint[] = "last_checkpoint";
const char __WT_CONFIG_CHOICE_oldest[] = "oldest";
const char __WT_CONFIG_CHOICE_oldest_reader[] = "oldest_reader";
const char __WT_CONFIG_CHOICE_oldest_timestamp[] = "oldest_timestamp";
const char __WT_CONFIG_CHOICE_pinned[] = "pinned";
const char __WT_CONFIG_CHOICE_recovery[] = "recovery";
const char __WT_CONFIG_CHOICE_stable[] = "stable";
const char __WT_CONFIG_CHOICE_stable_timestamp[] = "stable_timestamp";

static const char *confchk_get_choices[] = {__WT_CONFIG_CHOICE_all_durable,
  __WT_CONFIG_CHOICE_backup_checkpoint, __WT_CONFIG_CHOICE_last_checkpoint,
  __WT_CONFIG_CHOICE_oldest, __WT_CONFIG_CHOICE_oldest_reader, __WT_CONFIG_CHOICE_oldest_timestamp,
  __WT_CONFIG_CHOICE_pinned, __WT_CONFIG_CHOICE_recovery, __WT_CONFIG_CHOICE_stable,
  __WT_CONFIG_CHOICE_stable_timestamp, NULL};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_query_timestamp[] = {
  {"get", "string", NULL,
    "choices=[\"all_durable\",\"backup_checkpoint\","
    "\"last_checkpoint\",\"oldest\",\"oldest_reader\","
    "\"oldest_timestamp\",\"pinned\",\"recovery\",\"stable\","
    "\"stable_timestamp\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 149, INT64_MIN, INT64_MAX, confchk_get_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CONNECTION_query_timestamp_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_block_cache_subconfigs[] = {
  {"blkcache_eviction_aggression", "int", NULL, "min=1,max=7200", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 192, 1, 7200, NULL},
  {"cache_on_checkpoint", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    190, INT64_MIN, INT64_MAX, NULL},
  {"cache_on_writes", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 191,
    INT64_MIN, INT64_MAX, NULL},
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"full_target", "int", NULL, "min=30,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 193,
    30, 100, NULL},
  {"hashsize", "int", NULL, "min=512,max=256K", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 195,
    512, 256LL * WT_KILOBYTE, NULL},
  {"max_percent_overhead", "int", NULL, "min=1,max=500", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    196, 1, 500, NULL},
  {"nvram_path", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 197,
    INT64_MIN, INT64_MAX, NULL},
  {"percent_file_in_dram", "int", NULL, "min=0,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    198, 0, 100, NULL},
  {"size", "int", NULL, "min=0,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 194, 0,
    10LL * WT_TERABYTE, NULL},
  {"system_ram", "int", NULL, "min=0,max=1024GB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 199,
    0, 1024LL * WT_GIGABYTE, NULL},
  {"type", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 9, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_block_cache_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3,
    3, 4, 5, 5, 6, 6, 6, 6, 6, 7, 8, 8, 9, 9, 9, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_cache_eviction_controls_subconfigs[] = {
  {"incremental_app_eviction", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 201, INT64_MIN, INT64_MAX, NULL},
  {"scrub_evict_under_target_limit", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 202, INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_cache_eviction_controls_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_checkpoint_subconfigs[] = {
  {"log_size", "int", NULL, "min=0,max=2GB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 208, 0,
    2LL * WT_GIGABYTE, NULL},
  {"wait", "int", NULL, "min=0,max=100000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 209, 0,
    100000, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_checkpoint_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] =
  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2};
const char __WT_CONFIG_CHOICE_none[] = "none";
const char __WT_CONFIG_CHOICE_reclaim_space[] = "reclaim_space";

static const char *confchk_method_choices[] = {
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_reclaim_space, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_checkpoint_cleanup_subconfigs[] = {
  {"file_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 212, 0,
    INT64_MAX, NULL},
  {"method", "string", NULL, "choices=[\"none\",\"reclaim_space\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 211, INT64_MIN, INT64_MAX, confchk_method_choices},
  {"wait", "int", NULL, "min=1,max=100000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 209, 1,
    100000, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_checkpoint_cleanup_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure_chunk_cache_subconfigs[] = {
  {"pinned", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 214, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_CONNECTION_reconfigure_chunk_cache_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure_compatibility_subconfigs[] = {
  {"release", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 216, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_CONNECTION_reconfigure_compatibility_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_debug_mode_subconfigs[] = {
  {"background_compact", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 218,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_retention", "int", NULL, "min=0,max=1024", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 220, 0, 1024, NULL},
  {"configuration", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 221,
    INT64_MIN, INT64_MAX, NULL},
  {"corruption_abort", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 219,
    INT64_MIN, INT64_MAX, NULL},
  {"cursor_copy", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 222,
    INT64_MIN, INT64_MAX, NULL},
  {"cursor_reposition", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 223,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 224,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction_checkpoint_ts_ordering", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 235, INT64_MIN, INT64_MAX, NULL},
  {"log_retention", "int", NULL, "min=0,max=1024", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 225,
    0, 1024, NULL},
  {"page_history", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 226,
    INT64_MIN, INT64_MAX, NULL},
  {"realloc_exact", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 227,
    INT64_MIN, INT64_MAX, NULL},
  {"realloc_malloc", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 228,
    INT64_MIN, INT64_MAX, NULL},
  {"rollback_error", "int", NULL, "min=0,max=10M", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 229,
    0, 10LL * WT_MEGABYTE, NULL},
  {"slow_checkpoint", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 230,
    INT64_MIN, INT64_MAX, NULL},
  {"stress_skiplist", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 231,
    INT64_MIN, INT64_MAX, NULL},
  {"table_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 232,
    INT64_MIN, INT64_MAX, NULL},
  {"tiered_flush_error_continue", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 233, INT64_MIN, INT64_MAX, NULL},
  {"update_restore_evict", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    234, INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_debug_mode_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] =
  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 6, 6, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 13, 15, 17, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18};
const char __WT_CONFIG_CHOICE_delete[] = "delete";
const char __WT_CONFIG_CHOICE_fail[] = "fail";
const char __WT_CONFIG_CHOICE_ignore[] = "ignore";

static const char *confchk_local_files_action_choices[] = {
  __WT_CONFIG_CHOICE_delete, __WT_CONFIG_CHOICE_fail, __WT_CONFIG_CHOICE_ignore, NULL};
const char __WT_CONFIG_CHOICE_leader[] = "leader";
const char __WT_CONFIG_CHOICE_follower[] = "follower";

static const char *confchk_role_choices[] = {
  __WT_CONFIG_CHOICE_leader, __WT_CONFIG_CHOICE_follower, NULL};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure_disaggregated_subconfigs[] = {
  {"checkpoint_meta", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 74,
    INT64_MIN, INT64_MAX, NULL},
  {"last_materialized_lsn", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 75,
    INT64_MIN, INT64_MAX, NULL},
  {"local_files_action", "string", NULL, "choices=[\"delete\",\"fail\",\"ignore\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 76, INT64_MIN, INT64_MAX, confchk_local_files_action_choices},
  {"lose_all_my_data", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 77,
    INT64_MIN, INT64_MAX, NULL},
  {"role", "string", NULL, "choices=[\"leader\",\"follower\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 78, INT64_MIN, INT64_MAX, confchk_role_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_CONNECTION_reconfigure_disaggregated_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_eviction_subconfigs[] = {
  {"evict_sample_inmem", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 240,
    INT64_MIN, INT64_MAX, NULL},
  {"evict_use_softptr", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 241,
    INT64_MIN, INT64_MAX, NULL},
  {"legacy_page_visit_strategy", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 242, INT64_MIN, INT64_MAX, NULL},
  {"threads_max", "int", NULL, "min=1,max=64", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 238, 1,
    64, NULL},
  {"threads_min", "int", NULL, "min=1,max=64", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 239, 1,
    64, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_eviction_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
const char __WT_CONFIG_CHOICE_all[] = "all";
const char __WT_CONFIG_CHOICE_checkpoint_validate[] = "checkpoint_validate";
const char __WT_CONFIG_CHOICE_cursor_check[] = "cursor_check";
const char __WT_CONFIG_CHOICE_disk_validate[] = "disk_validate";
const char __WT_CONFIG_CHOICE_eviction_check[] = "eviction_check";
const char __WT_CONFIG_CHOICE_generation_check[] = "generation_check";
const char __WT_CONFIG_CHOICE_hs_validate[] = "hs_validate";
const char __WT_CONFIG_CHOICE_key_out_of_order[] = "key_out_of_order";
const char __WT_CONFIG_CHOICE_log_validate[] = "log_validate";
const char __WT_CONFIG_CHOICE_prepared[] = "prepared";
const char __WT_CONFIG_CHOICE_slow_operation[] = "slow_operation";
const char __WT_CONFIG_CHOICE_txn_visibility[] = "txn_visibility";

static const char *confchk_extra_diagnostics_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_checkpoint_validate, __WT_CONFIG_CHOICE_cursor_check,
  __WT_CONFIG_CHOICE_disk_validate, __WT_CONFIG_CHOICE_eviction_check,
  __WT_CONFIG_CHOICE_generation_check, __WT_CONFIG_CHOICE_hs_validate,
  __WT_CONFIG_CHOICE_key_out_of_order, __WT_CONFIG_CHOICE_log_validate, __WT_CONFIG_CHOICE_prepared,
  __WT_CONFIG_CHOICE_slow_operation, __WT_CONFIG_CHOICE_txn_visibility, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_file_manager_subconfigs[] = {
  {"close_handle_minimum", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 252, 0,
    INT64_MAX, NULL},
  {"close_idle_time", "int", NULL, "min=0,max=100000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    253, 0, 100000, NULL},
  {"close_scan_interval", "int", NULL, "min=1,max=100000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 254, 1, 100000, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_file_manager_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_heuristic_controls_subconfigs[] = {
  {"checkpoint_cleanup_obsolete_tw_pages_dirty_max", "int", NULL, "min=0,max=100000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 257, 0, 100000, NULL},
  {"eviction_obsolete_tw_pages_dirty_max", "int", NULL, "min=0,max=100000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 258, 0, 100000, NULL},
  {"obsolete_tw_btree_max", "int", NULL, "min=0,max=500000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 259, 0, 500000, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_heuristic_controls_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_history_store_subconfigs[] = {
  {"file_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 261, 0, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_history_store_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_io_capacity_subconfigs[] = {
  {"chunk_cache", "int", NULL, "min=0,max=1TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 264, 0,
    1LL * WT_TERABYTE, NULL},
  {"total", "int", NULL, "min=0,max=1TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 263, 0,
    1LL * WT_TERABYTE, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_io_capacity_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
const char __WT_CONFIG_CHOICE_error[] = "error";
const char __WT_CONFIG_CHOICE_message[] = "message";

static const char *confchk_json_output_choices[] = {
  __WT_CONFIG_CHOICE_error, __WT_CONFIG_CHOICE_message, NULL};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure_log_subconfigs[] = {
  {"archive", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 266, INT64_MIN,
    INT64_MAX, NULL},
  {"os_cache_dirty_pct", "int", NULL, "min=0,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    267, 0, 100, NULL},
  {"prealloc", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 268,
    INT64_MIN, INT64_MAX, NULL},
  {"prealloc_init_count", "int", NULL, "min=1,max=500", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    269, 1, 500, NULL},
  {"remove", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 270, INT64_MIN,
    INT64_MAX, NULL},
  {"zero_fill", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 271,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_CONNECTION_reconfigure_log_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_operation_tracking_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"path", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 273, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_operation_tracking_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure_page_delta_subconfigs[] = {
  {"delta_pct", "int", NULL, "min=1,max=1000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 275, 1,
    1000, NULL},
  {"flatten_leaf_page_delta", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    276, INT64_MIN, INT64_MAX, NULL},
  {"internal_page_delta", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    277, INT64_MIN, INT64_MAX, NULL},
  {"leaf_page_delta", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 278,
    INT64_MIN, INT64_MAX, NULL},
  {"max_consecutive_delta", "int", NULL, "min=1,max=32", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    279, 1, 32, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_CONNECTION_reconfigure_page_delta_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_rollback_to_stable_subconfigs[] = {
  {"threads", "int", NULL, "min=0,max=10", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 281, 0, 10,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_rollback_to_stable_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_shared_cache_subconfigs[] = {
  {"chunk", "int", NULL, "min=1MB,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 283,
    1LL * WT_MEGABYTE, 10LL * WT_TERABYTE, NULL},
  {"name", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 23, INT64_MIN,
    INT64_MAX, NULL},
  {"quota", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 284, INT64_MIN,
    INT64_MAX, NULL},
  {"reserve", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 285, INT64_MIN,
    INT64_MAX, NULL},
  {"size", "int", NULL, "min=1MB,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 194,
    1LL * WT_MEGABYTE, 10LL * WT_TERABYTE, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_shared_cache_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
const char __WT_CONFIG_CHOICE_cache_walk[] = "cache_walk";
const char __WT_CONFIG_CHOICE_fast[] = "fast";
const char __WT_CONFIG_CHOICE_clear[] = "clear";
const char __WT_CONFIG_CHOICE_tree_walk[] = "tree_walk";

static const char *confchk_statistics_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_cache_walk, __WT_CONFIG_CHOICE_fast, __WT_CONFIG_CHOICE_none,
  __WT_CONFIG_CHOICE_clear, __WT_CONFIG_CHOICE_tree_walk, NULL};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure_statistics_log_subconfigs[] = {
  {"json", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 287, INT64_MIN,
    INT64_MAX, NULL},
  {"on_close", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 288,
    INT64_MIN, INT64_MAX, NULL},
  {"sources", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 289, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 290, INT64_MIN,
    INT64_MAX, NULL},
  {"wait", "int", NULL, "min=0,max=100000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 209, 0,
    100000, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_CONNECTION_reconfigure_statistics_log_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure_tiered_storage_subconfigs[] = {
  {"local_retention", "int", NULL, "min=0,max=10000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    56, 0, 10000, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_CONNECTION_reconfigure_tiered_storage_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
const char __WT_CONFIG_CHOICE_aggressive_stash_free[] = "aggressive_stash_free";
const char __WT_CONFIG_CHOICE_aggressive_sweep[] = "aggressive_sweep";
const char __WT_CONFIG_CHOICE_backup_rename[] = "backup_rename";
const char __WT_CONFIG_CHOICE_checkpoint_evict_page[] = "checkpoint_evict_page";
const char __WT_CONFIG_CHOICE_checkpoint_handle[] = "checkpoint_handle";
const char __WT_CONFIG_CHOICE_checkpoint_slow[] = "checkpoint_slow";
const char __WT_CONFIG_CHOICE_checkpoint_stop[] = "checkpoint_stop";
const char __WT_CONFIG_CHOICE_commit_transaction_slow[] = "commit_transaction_slow";
const char __WT_CONFIG_CHOICE_compact_slow[] = "compact_slow";
const char __WT_CONFIG_CHOICE_conn_close_stress_log_printf[] = "conn_close_stress_log_printf";
const char __WT_CONFIG_CHOICE_evict_reposition[] = "evict_reposition";
const char __WT_CONFIG_CHOICE_failpoint_eviction_split[] = "failpoint_eviction_split";
const char __WT_CONFIG_CHOICE_failpoint_history_store_delete_key_from_ts[] =
  "failpoint_history_store_delete_key_from_ts";
const char __WT_CONFIG_CHOICE_history_store_checkpoint_delay[] = "history_store_checkpoint_delay";
const char __WT_CONFIG_CHOICE_history_store_search[] = "history_store_search";
const char __WT_CONFIG_CHOICE_history_store_sweep_race[] = "history_store_sweep_race";
const char __WT_CONFIG_CHOICE_live_restore_clean_up[] = "live_restore_clean_up";
const char __WT_CONFIG_CHOICE_open_index_slow[] = "open_index_slow";
const char __WT_CONFIG_CHOICE_prefetch_1[] = "prefetch_1";
const char __WT_CONFIG_CHOICE_prefetch_2[] = "prefetch_2";
const char __WT_CONFIG_CHOICE_prefetch_3[] = "prefetch_3";
const char __WT_CONFIG_CHOICE_prefix_compare[] = "prefix_compare";
const char __WT_CONFIG_CHOICE_prepare_checkpoint_delay[] = "prepare_checkpoint_delay";
const char __WT_CONFIG_CHOICE_prepare_resolution_1[] = "prepare_resolution_1";
const char __WT_CONFIG_CHOICE_prepare_resolution_2[] = "prepare_resolution_2";
const char __WT_CONFIG_CHOICE_session_alter_slow[] = "session_alter_slow";
const char __WT_CONFIG_CHOICE_sleep_before_read_overflow_onpage[] =
  "sleep_before_read_overflow_onpage";
const char __WT_CONFIG_CHOICE_split_1[] = "split_1";
const char __WT_CONFIG_CHOICE_split_2[] = "split_2";
const char __WT_CONFIG_CHOICE_split_3[] = "split_3";
const char __WT_CONFIG_CHOICE_split_4[] = "split_4";
const char __WT_CONFIG_CHOICE_split_5[] = "split_5";
const char __WT_CONFIG_CHOICE_split_6[] = "split_6";
const char __WT_CONFIG_CHOICE_split_7[] = "split_7";
const char __WT_CONFIG_CHOICE_split_8[] = "split_8";
const char __WT_CONFIG_CHOICE_tiered_flush_finish[] = "tiered_flush_finish";

static const char *confchk_timing_stress_for_test_choices[] = {
  __WT_CONFIG_CHOICE_aggressive_stash_free, __WT_CONFIG_CHOICE_aggressive_sweep,
  __WT_CONFIG_CHOICE_backup_rename, __WT_CONFIG_CHOICE_checkpoint_evict_page,
  __WT_CONFIG_CHOICE_checkpoint_handle, __WT_CONFIG_CHOICE_checkpoint_slow,
  __WT_CONFIG_CHOICE_checkpoint_stop, __WT_CONFIG_CHOICE_commit_transaction_slow,
  __WT_CONFIG_CHOICE_compact_slow, __WT_CONFIG_CHOICE_conn_close_stress_log_printf,
  __WT_CONFIG_CHOICE_evict_reposition, __WT_CONFIG_CHOICE_failpoint_eviction_split,
  __WT_CONFIG_CHOICE_failpoint_history_store_delete_key_from_ts,
  __WT_CONFIG_CHOICE_history_store_checkpoint_delay, __WT_CONFIG_CHOICE_history_store_search,
  __WT_CONFIG_CHOICE_history_store_sweep_race, __WT_CONFIG_CHOICE_live_restore_clean_up,
  __WT_CONFIG_CHOICE_open_index_slow, __WT_CONFIG_CHOICE_prefetch_1, __WT_CONFIG_CHOICE_prefetch_2,
  __WT_CONFIG_CHOICE_prefetch_3, __WT_CONFIG_CHOICE_prefix_compare,
  __WT_CONFIG_CHOICE_prepare_checkpoint_delay, __WT_CONFIG_CHOICE_prepare_resolution_1,
  __WT_CONFIG_CHOICE_prepare_resolution_2, __WT_CONFIG_CHOICE_session_alter_slow,
  __WT_CONFIG_CHOICE_sleep_before_read_overflow_onpage, __WT_CONFIG_CHOICE_split_1,
  __WT_CONFIG_CHOICE_split_2, __WT_CONFIG_CHOICE_split_3, __WT_CONFIG_CHOICE_split_4,
  __WT_CONFIG_CHOICE_split_5, __WT_CONFIG_CHOICE_split_6, __WT_CONFIG_CHOICE_split_7,
  __WT_CONFIG_CHOICE_split_8, __WT_CONFIG_CHOICE_tiered_flush_finish, NULL};
const char __WT_CONFIG_CHOICE_api[] = "api";
const char __WT_CONFIG_CHOICE_backup[] = "backup";
const char __WT_CONFIG_CHOICE_block[] = "block";
const char __WT_CONFIG_CHOICE_block_cache[] = "block_cache";
const char __WT_CONFIG_CHOICE_checkpoint[] = "checkpoint";
const char __WT_CONFIG_CHOICE_checkpoint_cleanup[] = "checkpoint_cleanup";
const char __WT_CONFIG_CHOICE_checkpoint_progress[] = "checkpoint_progress";
const char __WT_CONFIG_CHOICE_chunkcache[] = "chunkcache";
const char __WT_CONFIG_CHOICE_compact[] = "compact";
const char __WT_CONFIG_CHOICE_compact_progress[] = "compact_progress";
const char __WT_CONFIG_CHOICE_configuration[] = "configuration";
const char __WT_CONFIG_CHOICE_disaggregated_storage[] = "disaggregated_storage";
const char __WT_CONFIG_CHOICE_error_returns[] = "error_returns";
const char __WT_CONFIG_CHOICE_eviction[] = "eviction";
const char __WT_CONFIG_CHOICE_fileops[] = "fileops";
const char __WT_CONFIG_CHOICE_generation[] = "generation";
const char __WT_CONFIG_CHOICE_handleops[] = "handleops";
const char __WT_CONFIG_CHOICE_history_store[] = "history_store";
const char __WT_CONFIG_CHOICE_history_store_activity[] = "history_store_activity";
const char __WT_CONFIG_CHOICE_layered[] = "layered";
const char __WT_CONFIG_CHOICE_live_restore[] = "live_restore";
const char __WT_CONFIG_CHOICE_live_restore_progress[] = "live_restore_progress";
const char __WT_CONFIG_CHOICE_log[] = "log";
const char __WT_CONFIG_CHOICE_metadata[] = "metadata";
const char __WT_CONFIG_CHOICE_mutex[] = "mutex";
const char __WT_CONFIG_CHOICE_out_of_order[] = "out_of_order";
const char __WT_CONFIG_CHOICE_overflow[] = "overflow";
const char __WT_CONFIG_CHOICE_page_delta[] = "page_delta";
const char __WT_CONFIG_CHOICE_prefetch[] = "prefetch";
const char __WT_CONFIG_CHOICE_read[] = "read";
const char __WT_CONFIG_CHOICE_reconcile[] = "reconcile";
const char __WT_CONFIG_CHOICE_recovery_progress[] = "recovery_progress";
const char __WT_CONFIG_CHOICE_rts[] = "rts";
const char __WT_CONFIG_CHOICE_salvage[] = "salvage";
const char __WT_CONFIG_CHOICE_shared_cache[] = "shared_cache";
const char __WT_CONFIG_CHOICE_split[] = "split";
const char __WT_CONFIG_CHOICE_sweep[] = "sweep";
const char __WT_CONFIG_CHOICE_temporary[] = "temporary";
const char __WT_CONFIG_CHOICE_thread_group[] = "thread_group";
const char __WT_CONFIG_CHOICE_tiered[] = "tiered";
const char __WT_CONFIG_CHOICE_timestamp[] = "timestamp";
const char __WT_CONFIG_CHOICE_transaction[] = "transaction";
const char __WT_CONFIG_CHOICE_verify[] = "verify";
const char __WT_CONFIG_CHOICE_version[] = "version";
const char __WT_CONFIG_CHOICE_write[] = "write";

static const char *confchk_verbose_choices[] = {__WT_CONFIG_CHOICE_all, __WT_CONFIG_CHOICE_api,
  __WT_CONFIG_CHOICE_backup, __WT_CONFIG_CHOICE_block, __WT_CONFIG_CHOICE_block_cache,
  __WT_CONFIG_CHOICE_checkpoint, __WT_CONFIG_CHOICE_checkpoint_cleanup,
  __WT_CONFIG_CHOICE_checkpoint_progress, __WT_CONFIG_CHOICE_chunkcache, __WT_CONFIG_CHOICE_compact,
  __WT_CONFIG_CHOICE_compact_progress, __WT_CONFIG_CHOICE_configuration,
  __WT_CONFIG_CHOICE_disaggregated_storage, __WT_CONFIG_CHOICE_error_returns,
  __WT_CONFIG_CHOICE_eviction, __WT_CONFIG_CHOICE_fileops, __WT_CONFIG_CHOICE_generation,
  __WT_CONFIG_CHOICE_handleops, __WT_CONFIG_CHOICE_history_store,
  __WT_CONFIG_CHOICE_history_store_activity, __WT_CONFIG_CHOICE_layered,
  __WT_CONFIG_CHOICE_live_restore, __WT_CONFIG_CHOICE_live_restore_progress, __WT_CONFIG_CHOICE_log,
  __WT_CONFIG_CHOICE_metadata, __WT_CONFIG_CHOICE_mutex, __WT_CONFIG_CHOICE_out_of_order,
  __WT_CONFIG_CHOICE_overflow, __WT_CONFIG_CHOICE_page_delta, __WT_CONFIG_CHOICE_prefetch,
  __WT_CONFIG_CHOICE_read, __WT_CONFIG_CHOICE_reconcile, __WT_CONFIG_CHOICE_recovery,
  __WT_CONFIG_CHOICE_recovery_progress, __WT_CONFIG_CHOICE_rts, __WT_CONFIG_CHOICE_salvage,
  __WT_CONFIG_CHOICE_shared_cache, __WT_CONFIG_CHOICE_split, __WT_CONFIG_CHOICE_sweep,
  __WT_CONFIG_CHOICE_temporary, __WT_CONFIG_CHOICE_thread_group, __WT_CONFIG_CHOICE_tiered,
  __WT_CONFIG_CHOICE_timestamp, __WT_CONFIG_CHOICE_transaction, __WT_CONFIG_CHOICE_verify,
  __WT_CONFIG_CHOICE_version, __WT_CONFIG_CHOICE_write, NULL};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_reconfigure[] = {
  {"block_cache", "category", NULL, NULL, confchk_wiredtiger_open_block_cache_subconfigs, 12,
    confchk_wiredtiger_open_block_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 189,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_eviction_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs, 2,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 200, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 203, 0,
    INT64_MAX, NULL},
  {"cache_overhead", "int", NULL, "min=0,max=30", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 204,
    0, 30, NULL},
  {"cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 205,
    1LL * WT_MEGABYTE, 10LL * WT_TERABYTE, NULL},
  {"cache_stuck_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 206,
    0, INT64_MAX, NULL},
  {"checkpoint", "category", NULL, NULL, confchk_wiredtiger_open_checkpoint_subconfigs, 2,
    confchk_wiredtiger_open_checkpoint_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 207,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_cleanup", "category", NULL, NULL,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs, 3,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    210, INT64_MIN, INT64_MAX, NULL},
  {"chunk_cache", "category", NULL, NULL, confchk_WT_CONNECTION_reconfigure_chunk_cache_subconfigs,
    1, confchk_WT_CONNECTION_reconfigure_chunk_cache_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 213, INT64_MIN, INT64_MAX, NULL},
  {"compatibility", "category", NULL, NULL,
    confchk_WT_CONNECTION_reconfigure_compatibility_subconfigs, 1,
    confchk_WT_CONNECTION_reconfigure_compatibility_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 215, INT64_MIN, INT64_MAX, NULL},
  {"debug_mode", "category", NULL, NULL, confchk_wiredtiger_open_debug_mode_subconfigs, 18,
    confchk_wiredtiger_open_debug_mode_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 217,
    INT64_MIN, INT64_MAX, NULL},
  {"disaggregated", "category", NULL, NULL,
    confchk_WT_CONNECTION_reconfigure_disaggregated_subconfigs, 5,
    confchk_WT_CONNECTION_reconfigure_disaggregated_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 20, INT64_MIN, INT64_MAX, NULL},
  {"error_prefix", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 236,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction", "category", NULL, NULL, confchk_wiredtiger_open_eviction_subconfigs, 5,
    confchk_wiredtiger_open_eviction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 237,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction_checkpoint_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 243, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_target", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 244, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_trigger", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 245, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    246, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_trigger", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    247, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 248, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_trigger", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 249, 0, 10LL * WT_TERABYTE, NULL},
  {"extra_diagnostics", "list", NULL,
    "choices=[\"all\",\"checkpoint_validate\",\"cursor_check\""
    ",\"disk_validate\",\"eviction_check\",\"generation_check\","
    "\"hs_validate\",\"key_out_of_order\",\"log_validate\","
    "\"prepared\",\"slow_operation\",\"txn_visibility\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 250, INT64_MIN, INT64_MAX,
    confchk_extra_diagnostics_choices},
  {"file_manager", "category", NULL, NULL, confchk_wiredtiger_open_file_manager_subconfigs, 3,
    confchk_wiredtiger_open_file_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 251,
    INT64_MIN, INT64_MAX, NULL},
  {"generation_drain_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    255, 0, INT64_MAX, NULL},
  {"heuristic_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_heuristic_controls_subconfigs, 3,
    confchk_wiredtiger_open_heuristic_controls_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    256, INT64_MIN, INT64_MAX, NULL},
  {"history_store", "category", NULL, NULL, confchk_wiredtiger_open_history_store_subconfigs, 1,
    confchk_wiredtiger_open_history_store_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 260,
    INT64_MIN, INT64_MAX, NULL},
  {"io_capacity", "category", NULL, NULL, confchk_wiredtiger_open_io_capacity_subconfigs, 2,
    confchk_wiredtiger_open_io_capacity_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 262,
    INT64_MIN, INT64_MAX, NULL},
  {"json_output", "list", NULL, "choices=[\"error\",\"message\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 265, INT64_MIN, INT64_MAX, confchk_json_output_choices},
  {"log", "category", NULL, NULL, confchk_WT_CONNECTION_reconfigure_log_subconfigs, 6,
    confchk_WT_CONNECTION_reconfigure_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40,
    INT64_MIN, INT64_MAX, NULL},
  {"operation_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 166, 0,
    INT64_MAX, NULL},
  {"operation_tracking", "category", NULL, NULL,
    confchk_wiredtiger_open_operation_tracking_subconfigs, 2,
    confchk_wiredtiger_open_operation_tracking_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    272, INT64_MIN, INT64_MAX, NULL},
  {"page_delta", "category", NULL, NULL, confchk_WT_CONNECTION_reconfigure_page_delta_subconfigs, 5,
    confchk_WT_CONNECTION_reconfigure_page_delta_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    274, INT64_MIN, INT64_MAX, NULL},
  {"rollback_to_stable", "category", NULL, NULL,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs, 1,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    280, INT64_MIN, INT64_MAX, NULL},
  {"shared_cache", "category", NULL, NULL, confchk_wiredtiger_open_shared_cache_subconfigs, 5,
    confchk_wiredtiger_open_shared_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 282,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics", "list", NULL,
    "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
    "\"clear\",\"tree_walk\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 147, INT64_MIN, INT64_MAX,
    confchk_statistics_choices},
  {"statistics_log", "category", NULL, NULL,
    confchk_WT_CONNECTION_reconfigure_statistics_log_subconfigs, 5,
    confchk_WT_CONNECTION_reconfigure_statistics_log_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 286, INT64_MIN, INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL,
    confchk_WT_CONNECTION_reconfigure_tiered_storage_subconfigs, 1,
    confchk_WT_CONNECTION_reconfigure_tiered_storage_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 51, INT64_MIN, INT64_MAX, NULL},
  {"timing_stress_for_test", "list", NULL,
    "choices=[\"aggressive_stash_free\",\"aggressive_sweep\","
    "\"backup_rename\",\"checkpoint_evict_page\","
    "\"checkpoint_handle\",\"checkpoint_slow\",\"checkpoint_stop\","
    "\"commit_transaction_slow\",\"compact_slow\","
    "\"conn_close_stress_log_printf\",\"evict_reposition\","
    "\"failpoint_eviction_split\","
    "\"failpoint_history_store_delete_key_from_ts\","
    "\"history_store_checkpoint_delay\",\"history_store_search\","
    "\"history_store_sweep_race\",\"live_restore_clean_up\","
    "\"open_index_slow\",\"prefetch_1\",\"prefetch_2\",\"prefetch_3\""
    ",\"prefix_compare\",\"prepare_checkpoint_delay\","
    "\"prepare_resolution_1\",\"prepare_resolution_2\","
    "\"session_alter_slow\",\"sleep_before_read_overflow_onpage\","
    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
    "\"split_6\",\"split_7\",\"split_8\",\"tiered_flush_finish\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 291, INT64_MIN, INT64_MAX,
    confchk_timing_stress_for_test_choices},
  {"verbose", "list", NULL,
    "choices=[\"all\",\"api\",\"backup\",\"block\","
    "\"block_cache\",\"checkpoint\",\"checkpoint_cleanup\","
    "\"checkpoint_progress\",\"chunkcache\",\"compact\","
    "\"compact_progress\",\"configuration\",\"disaggregated_storage\""
    ",\"error_returns\",\"eviction\",\"fileops\",\"generation\","
    "\"handleops\",\"history_store\",\"history_store_activity\","
    "\"layered\",\"live_restore\",\"live_restore_progress\",\"log\","
    "\"metadata\",\"mutex\",\"out_of_order\",\"overflow\","
    "\"page_delta\",\"prefetch\",\"read\",\"reconcile\",\"recovery\","
    "\"recovery_progress\",\"rts\",\"salvage\",\"shared_cache\","
    "\"split\",\"sweep\",\"temporary\",\"thread_group\",\"tiered\","
    "\"timestamp\",\"transaction\",\"verify\",\"version\",\"write\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CONNECTION_reconfigure_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 10,
  12, 22, 23, 24, 26, 27, 28, 28, 29, 29, 29, 31, 32, 32, 33, 36, 38, 38, 39, 39, 39, 39, 39, 39,
  39, 39, 39};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_rollback_to_stable[] = {
  {"dryrun", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 93, INT64_MIN,
    INT64_MAX, NULL},
  {"threads", "int", NULL, "min=0,max=10", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 281, 0, 10,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CONNECTION_rollback_to_stable_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_WT_CONNECTION_set_timestamp[] = {
  {"durable_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 3,
    INT64_MIN, INT64_MAX, NULL},
  {"force", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 118, INT64_MIN,
    INT64_MAX, NULL},
  {"oldest_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 301,
    INT64_MIN, INT64_MAX, NULL},
  {"stable_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 160,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CONNECTION_set_timestamp_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
  1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
const char __WT_CONFIG_CHOICE_set[] = "set";

static const char *confchk_action_choices[] = {
  __WT_CONFIG_CHOICE_clear, __WT_CONFIG_CHOICE_set, NULL};
const char __WT_CONFIG_CHOICE_lower[] = "lower";
const char __WT_CONFIG_CHOICE_upper[] = "upper";

static const char *confchk_bound_choices[] = {
  __WT_CONFIG_CHOICE_lower, __WT_CONFIG_CHOICE_upper, NULL};

static const WT_CONFIG_CHECK confchk_WT_CURSOR_bound[] = {
  {"action", "string", NULL, "choices=[\"clear\",\"set\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 88, INT64_MIN, INT64_MAX, confchk_action_choices},
  {"bound", "string", NULL, "choices=[\"lower\",\"upper\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 89, INT64_MIN, INT64_MAX, confchk_bound_choices},
  {"inclusive", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 90,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CURSOR_bound_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 2, 2, 2, 2, 2, 2,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

static const WT_CONFIG_CHECK confchk_WT_CURSOR_reconfigure[] = {
  {"append", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 85, INT64_MIN,
    INT64_MAX, NULL},
  {"overwrite", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 86,
    INT64_MIN, INT64_MAX, NULL},
  {"prefix_search", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 87,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_CURSOR_reconfigure_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
const char __WT_CONFIG_CHOICE_random[] = "random";
const char __WT_CONFIG_CHOICE_sequential[] = "sequential";

static const char *confchk_access_pattern_hint_choices[] = {
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_random, __WT_CONFIG_CHOICE_sequential, NULL};
const char __WT_CONFIG_CHOICE_always[] = "always";
const char __WT_CONFIG_CHOICE_key_consistent[] = "key_consistent";
const char __WT_CONFIG_CHOICE_never[] = "never";

static const char *confchk_commit_timestamp_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_never, __WT_CONFIG_CHOICE_none, NULL};

static const char *confchk_durable_timestamp_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_never, __WT_CONFIG_CHOICE_none, NULL};

static const char *confchk_read_timestamp_choices[] = {
  __WT_CONFIG_CHOICE_always, __WT_CONFIG_CHOICE_never, __WT_CONFIG_CHOICE_none, NULL};
const char __WT_CONFIG_CHOICE_off[] = "off";
const char __WT_CONFIG_CHOICE_on[] = "on";

static const char *confchk_write_timestamp_choices[] = {
  __WT_CONFIG_CHOICE_off, __WT_CONFIG_CHOICE_on, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_create_assert_subconfigs[] = {
  {"commit_timestamp", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"never\","
    "\"none\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 2, INT64_MIN, INT64_MAX,
    confchk_commit_timestamp_choices},
  {"durable_timestamp", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"never\","
    "\"none\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 3, INT64_MIN, INT64_MAX,
    confchk_durable_timestamp_choices},
  {"read_timestamp", "string", NULL, "choices=[\"always\",\"never\",\"none\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 4, INT64_MIN, INT64_MAX, confchk_read_timestamp_choices},
  {"write_timestamp", "string", NULL, "choices=[\"off\",\"on\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 5, INT64_MIN, INT64_MAX, confchk_write_timestamp_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_create_assert_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4};

static const WT_CONFIG_CHECK confchk_WT_SESSION_create_log_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_create_log_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
const char __WT_CONFIG_CHOICE_write_timestamp[] = "write_timestamp";

static const char *confchk_verbose2_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};
const char __WT_CONFIG_CHOICE_mixed_mode[] = "mixed_mode";
const char __WT_CONFIG_CHOICE_ordered[] = "ordered";

static const char *confchk_write_timestamp_usage_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_alter[] = {
  {"access_pattern_hint", "string", NULL, "choices=[\"none\",\"random\",\"sequential\"]", NULL, 0,
    NULL, WT_CONFIG_COMPILED_TYPE_STRING, 12, INT64_MIN, INT64_MAX,
    confchk_access_pattern_hint_choices},
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_resident", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 17,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 60, INT64_MIN,
    INT64_MAX, NULL},
  {"exclusive_refreshed", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 91,
    INT64_MIN, INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_WT_SESSION_create_log_subconfigs, 1,
    confchk_WT_SESSION_create_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"os_cache_dirty_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 44, 0,
    INT64_MAX, NULL},
  {"os_cache_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 45, 0,
    INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose2_choices},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_alter_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 5, 5, 6, 6, 6,
  6, 6, 6, 6, 7, 7, 7, 9, 9, 9, 9, 9, 9, 9, 10, 11, 11, 11, 11, 11, 11, 11, 11};
const char __WT_CONFIG_CHOICE_false[] = "false";
const char __WT_CONFIG_CHOICE_force[] = "force";
const char __WT_CONFIG_CHOICE_true[] = "true";

static const char *confchk_ignore_prepare_choices[] = {
  __WT_CONFIG_CHOICE_false, __WT_CONFIG_CHOICE_force, __WT_CONFIG_CHOICE_true, NULL};

static const char *confchk_isolation2_choices[] = {__WT_CONFIG_CHOICE_read_uncommitted,
  __WT_CONFIG_CHOICE_read_committed, __WT_CONFIG_CHOICE_snapshot, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_begin_transaction_roundup_timestamps_subconfigs[] =
  {{"prepared", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 169,
     INT64_MIN, INT64_MAX, NULL},
    {"read", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 170, INT64_MIN,
      INT64_MAX, NULL},
    {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_begin_transaction_roundup_timestamps_subconfigs_jump
  [WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_WT_SESSION_begin_transaction[] = {
  {"claim_prepared_id", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 162,
    INT64_MIN, INT64_MAX, NULL},
  {"ignore_prepare", "string", NULL, "choices=[\"false\",\"force\",\"true\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 163, INT64_MIN, INT64_MAX, confchk_ignore_prepare_choices},
  {"isolation", "string", NULL,
    "choices=[\"read-uncommitted\",\"read-committed\","
    "\"snapshot\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 164, INT64_MIN, INT64_MAX,
    confchk_isolation2_choices},
  {"name", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 23, INT64_MIN,
    INT64_MAX, NULL},
  {"no_timestamp", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 165,
    INT64_MIN, INT64_MAX, NULL},
  {"operation_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 166, 0,
    INT64_MAX, NULL},
  {"priority", "int", NULL, "min=-100,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 167,
    -100, 100, NULL},
  {"read_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 4,
    INT64_MIN, INT64_MAX, NULL},
  {"roundup_timestamps", "category", NULL, NULL,
    confchk_WT_SESSION_begin_transaction_roundup_timestamps_subconfigs, 2,
    confchk_WT_SESSION_begin_transaction_roundup_timestamps_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 168, INT64_MIN, INT64_MAX, NULL},
  {"sync", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 122, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_begin_transaction_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
  1, 1, 1, 1, 1, 3, 3, 3, 3, 3, 5, 6, 7, 7, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};

static const WT_CONFIG_CHECK confchk_WT_SESSION_checkpoint_debug_subconfigs[] = {
  {"checkpoint_cleanup", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 174,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_crash_point", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 175,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_SESSION_checkpoint_debug_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_WT_SESSION_checkpoint_flush_tier_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"force", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 118, INT64_MIN,
    INT64_MAX, NULL},
  {"sync", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 122, INT64_MIN,
    INT64_MAX, NULL},
  {"timeout", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 97, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_SESSION_checkpoint_flush_tier_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

static const WT_CONFIG_CHECK confchk_WT_SESSION_checkpoint[] = {
  {"debug", "category", NULL, NULL, confchk_WT_SESSION_checkpoint_debug_subconfigs, 2,
    confchk_WT_SESSION_checkpoint_debug_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 125,
    INT64_MIN, INT64_MAX, NULL},
  {"drop", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 176, INT64_MIN,
    INT64_MAX, NULL},
  {"flush_tier", "category", NULL, NULL, confchk_WT_SESSION_checkpoint_flush_tier_subconfigs, 4,
    confchk_WT_SESSION_checkpoint_flush_tier_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 177,
    INT64_MIN, INT64_MAX, NULL},
  {"force", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 118, INT64_MIN,
    INT64_MAX, NULL},
  {"name", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 23, INT64_MIN,
    INT64_MAX, NULL},
  {"use_timestamp", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 178,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_checkpoint_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 4,
  4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};

static const char *confchk_sync_choices[] = {__WT_CONFIG_CHOICE_off, __WT_CONFIG_CHOICE_on, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_commit_transaction[] = {
  {"commit_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 2,
    INT64_MIN, INT64_MAX, NULL},
  {"durable_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 3,
    INT64_MIN, INT64_MAX, NULL},
  {"operation_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 166, 0,
    INT64_MAX, NULL},
  {"sync", "string", NULL, "choices=[\"off\",\"on\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 122, INT64_MIN, INT64_MAX, confchk_sync_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_commit_transaction_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

static const WT_CONFIG_CHECK confchk_WT_SESSION_compact[] = {
  {"background", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 92,
    INT64_MIN, INT64_MAX, NULL},
  {"dryrun", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 93, INT64_MIN,
    INT64_MAX, NULL},
  {"exclude", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 94, INT64_MIN,
    INT64_MAX, NULL},
  {"free_space_target", "int", NULL, "min=1MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 95,
    1LL * WT_MEGABYTE, INT64_MAX, NULL},
  {"run_once", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 96, INT64_MIN,
    INT64_MAX, NULL},
  {"timeout", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 97, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_compact_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};

static const char *confchk_access_pattern_hint2_choices[] = {
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_random, __WT_CONFIG_CHOICE_sequential, NULL};
const char __WT_CONFIG_CHOICE_best[] = "best";
const char __WT_CONFIG_CHOICE_first[] = "first";

static const char *confchk_block_allocation_choices[] = {
  __WT_CONFIG_CHOICE_best, __WT_CONFIG_CHOICE_first, NULL};
const char __WT_CONFIG_CHOICE_default[] = "default";
const char __WT_CONFIG_CHOICE_disagg[] = "disagg";

static const char *confchk_block_manager_choices[] = {
  __WT_CONFIG_CHOICE_default, __WT_CONFIG_CHOICE_disagg, NULL};
const char __WT_CONFIG_CHOICE_uncompressed[] = "uncompressed";
const char __WT_CONFIG_CHOICE_unencrypted[] = "unencrypted";

static const char *confchk_checksum_choices[] = {__WT_CONFIG_CHOICE_on, __WT_CONFIG_CHOICE_off,
  __WT_CONFIG_CHOICE_uncompressed, __WT_CONFIG_CHOICE_unencrypted, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_create_disaggregated_subconfigs[] = {
  {"page_log", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 21, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_SESSION_create_disaggregated_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const WT_CONFIG_CHECK confchk_WT_SESSION_create_encryption_subconfigs[] = {
  {"keyid", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 24, INT64_MIN,
    INT64_MAX, NULL},
  {"name", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 23, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_SESSION_create_encryption_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
const char __WT_CONFIG_CHOICE_btree[] = "btree";

static const char *confchk_format_choices[] = {__WT_CONFIG_CHOICE_btree, NULL};

static const char *confchk_compare_timestamp_choices[] = {__WT_CONFIG_CHOICE_oldest,
  __WT_CONFIG_CHOICE_oldest_timestamp, __WT_CONFIG_CHOICE_stable,
  __WT_CONFIG_CHOICE_stable_timestamp, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_create_import_subconfigs[] = {
  {"compare_timestamp", "string", NULL,
    "choices=[\"oldest\",\"oldest_timestamp\",\"stable\","
    "\"stable_timestamp\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 100, INT64_MIN, INT64_MAX,
    confchk_compare_timestamp_choices},
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"file_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 101,
    INT64_MIN, INT64_MAX, NULL},
  {"metadata_file", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 102,
    INT64_MIN, INT64_MAX, NULL},
  {"panic_corrupt", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 103,
    INT64_MIN, INT64_MAX, NULL},
  {"repair", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 104, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_create_import_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 1, 1, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};

static const WT_CONFIG_CHECK confchk_lsm_subconfigs[] = {
  {"auto_throttle", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 106,
    INT64_MIN, INT64_MAX, NULL},
  {"bloom", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 107, INT64_MIN,
    INT64_MAX, NULL},
  {"bloom_bit_count", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 108,
    INT64_MIN, INT64_MAX, NULL},
  {"bloom_config", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 109,
    INT64_MIN, INT64_MAX, NULL},
  {"bloom_hash_count", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 110,
    INT64_MIN, INT64_MAX, NULL},
  {"bloom_oldest", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 111,
    INT64_MIN, INT64_MAX, NULL},
  {"chunk_count_limit", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 112,
    INT64_MIN, INT64_MAX, NULL},
  {"chunk_max", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 113, INT64_MIN,
    INT64_MAX, NULL},
  {"chunk_size", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 114,
    INT64_MIN, INT64_MAX, NULL},
  {"merge_max", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 115, INT64_MIN,
    INT64_MAX, NULL},
  {"merge_min", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 116, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_lsm_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 6, 9, 9, 9, 9, 9, 9,
  9, 9, 9, 9, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11};

static const WT_CONFIG_CHECK confchk_WT_SESSION_create_tiered_storage_subconfigs[] = {
  {"auth_token", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 52, INT64_MIN,
    INT64_MAX, NULL},
  {"bucket", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 53, INT64_MIN,
    INT64_MAX, NULL},
  {"bucket_prefix", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 54,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_directory", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 55,
    INT64_MIN, INT64_MAX, NULL},
  {"local_retention", "int", NULL, "min=0,max=10000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    56, 0, 10000, NULL},
  {"name", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 23, INT64_MIN,
    INT64_MAX, NULL},
  {"object_target_size", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 57, 0,
    INT64_MAX, NULL},
  {"shared", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 58, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_SESSION_create_tiered_storage_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 6, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};

static const char *confchk_verbose3_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage2_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_create[] = {
  {"access_pattern_hint", "string", NULL, "choices=[\"none\",\"random\",\"sequential\"]", NULL, 0,
    NULL, WT_CONFIG_COMPILED_TYPE_STRING, 12, INT64_MIN, INT64_MAX,
    confchk_access_pattern_hint2_choices},
  {"allocation_size", "int", NULL, "min=512B,max=128MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    13, 512, 128LL * WT_MEGABYTE, NULL},
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"block_allocation", "string", NULL, "choices=[\"best\",\"first\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 14, INT64_MIN, INT64_MAX, confchk_block_allocation_choices},
  {"block_compressor", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 15,
    INT64_MIN, INT64_MAX, NULL},
  {"block_manager", "string", NULL, "choices=[\"default\",\"disagg\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 16, INT64_MIN, INT64_MAX, confchk_block_manager_choices},
  {"cache_resident", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 17,
    INT64_MIN, INT64_MAX, NULL},
  {"checksum", "string", NULL,
    "choices=[\"on\",\"off\",\"uncompressed\","
    "\"unencrypted\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 18, INT64_MIN, INT64_MAX,
    confchk_checksum_choices},
  {"colgroups", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 81, INT64_MIN,
    INT64_MAX, NULL},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"dictionary", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 19, 0, INT64_MAX,
    NULL},
  {"disaggregated", "category", NULL, NULL, confchk_WT_SESSION_create_disaggregated_subconfigs, 1,
    confchk_WT_SESSION_create_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_WT_SESSION_create_encryption_subconfigs, 2,
    confchk_WT_SESSION_create_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"exclusive", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 98,
    INT64_MIN, INT64_MAX, NULL},
  {"extractor", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 70, INT64_MIN,
    INT64_MAX, NULL},
  {"format", "string", NULL, "choices=[\"btree\"]", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    25, INT64_MIN, INT64_MAX, confchk_format_choices},
  {"huffman_key", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 26,
    INT64_MIN, INT64_MAX, NULL},
  {"huffman_value", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 27,
    INT64_MIN, INT64_MAX, NULL},
  {"ignore_in_memory_cache_size", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 28, INT64_MIN, INT64_MAX, NULL},
  {"immutable", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 71,
    INT64_MIN, INT64_MAX, NULL},
  {"import", "category", NULL, NULL, confchk_WT_SESSION_create_import_subconfigs, 6,
    confchk_WT_SESSION_create_import_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 99,
    INT64_MIN, INT64_MAX, NULL},
  {"in_memory", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 29,
    INT64_MIN, INT64_MAX, NULL},
  {"ingest", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 79, INT64_MIN,
    INT64_MAX, NULL},
  {"internal_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 30, 0,
    INT64_MAX, NULL},
  {"internal_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 31, 0,
    INT64_MAX, NULL},
  {"internal_key_truncate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    32, INT64_MIN, INT64_MAX, NULL},
  {"internal_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 33, 512, 512LL * WT_MEGABYTE, NULL},
  {"key_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_FORMAT,
    34, INT64_MIN, INT64_MAX, NULL},
  {"key_gap", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 35, 0, INT64_MAX,
    NULL},
  {"leaf_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 36, 0,
    INT64_MAX, NULL},
  {"leaf_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 37, 0,
    INT64_MAX, NULL},
  {"leaf_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    38, 512, 512LL * WT_MEGABYTE, NULL},
  {"leaf_value_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 39, 0,
    INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_WT_SESSION_create_log_subconfigs, 1,
    confchk_WT_SESSION_create_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"lsm", "category", NULL, NULL, confchk_lsm_subconfigs, 11, confchk_lsm_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 105, INT64_MIN, INT64_MAX, NULL},
  {"memory_page_image_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 42, 0,
    INT64_MAX, NULL},
  {"memory_page_max", "int", NULL, "min=512B,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    43, 512, 10LL * WT_TERABYTE, NULL},
  {"os_cache_dirty_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 44, 0,
    INT64_MAX, NULL},
  {"os_cache_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 45, 0,
    INT64_MAX, NULL},
  {"prefix_compression", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 46,
    INT64_MIN, INT64_MAX, NULL},
  {"prefix_compression_min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 47,
    0, INT64_MAX, NULL},
  {"source", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 8, INT64_MIN,
    INT64_MAX, NULL},
  {"split_deepen_min_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 48,
    INT64_MIN, INT64_MAX, NULL},
  {"split_deepen_per_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 49,
    INT64_MIN, INT64_MAX, NULL},
  {"split_pct", "int", NULL, "min=50,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 50, 50,
    100, NULL},
  {"stable", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 80, INT64_MIN,
    INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_WT_SESSION_create_tiered_storage_subconfigs, 8,
    confchk_WT_SESSION_create_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51,
    INT64_MIN, INT64_MAX, NULL},
  {"type", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 9, INT64_MIN,
    INT64_MAX, NULL},
  {"value_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_FORMAT, 59, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose3_choices},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage2_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_create_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 7, 12, 14, 17,
  18, 18, 20, 29, 29, 31, 37, 39, 39, 41, 43, 43, 43, 48, 50, 50, 52, 53, 53, 53, 53, 53, 53, 53,
  53};

static const WT_CONFIG_CHECK confchk_WT_SESSION_drop[] = {
  {"checkpoint_wait", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 117,
    INT64_MIN, INT64_MAX, NULL},
  {"force", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 118, INT64_MIN,
    INT64_MAX, NULL},
  {"lock_wait", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 119,
    INT64_MIN, INT64_MAX, NULL},
  {"remove_files", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 120,
    INT64_MIN, INT64_MAX, NULL},
  {"remove_shared", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 121,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_drop_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2,
  2, 2, 2, 3, 3, 3, 3, 3, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const char *confchk_sync2_choices[] = {__WT_CONFIG_CHOICE_off, __WT_CONFIG_CHOICE_on, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_log_flush[] = {
  {"sync", "string", NULL, "choices=[\"off\",\"on\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 122, INT64_MIN, INT64_MAX, confchk_sync2_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_log_flush_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const WT_CONFIG_CHECK confchk_WT_SESSION_open_cursor_dump_version_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"raw_key_value", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 131,
    INT64_MIN, INT64_MAX, NULL},
  {"start_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 129,
    INT64_MIN, INT64_MAX, NULL},
  {"timestamp_order", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 130,
    INT64_MIN, INT64_MAX, NULL},
  {"visible_only", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 128,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_SESSION_open_cursor_dump_version_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_WT_SESSION_open_cursor_debug_subconfigs[] = {
  {"checkpoint_read_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    126, INT64_MIN, INT64_MAX, NULL},
  {"dump_version", "category", NULL, NULL, confchk_WT_SESSION_open_cursor_dump_version_subconfigs,
    5, confchk_WT_SESSION_open_cursor_dump_version_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 127, INT64_MIN, INT64_MAX, NULL},
  {"release_evict", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 132,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_SESSION_open_cursor_debug_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
const char __WT_CONFIG_CHOICE_hex[] = "hex";
const char __WT_CONFIG_CHOICE_json[] = "json";
const char __WT_CONFIG_CHOICE_pretty[] = "pretty";
const char __WT_CONFIG_CHOICE_pretty_hex[] = "pretty_hex";
const char __WT_CONFIG_CHOICE_print[] = "print";

static const char *confchk_dump_choices[] = {__WT_CONFIG_CHOICE_hex, __WT_CONFIG_CHOICE_json,
  __WT_CONFIG_CHOICE_pretty, __WT_CONFIG_CHOICE_pretty_hex, __WT_CONFIG_CHOICE_print, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_open_cursor_incremental_subconfigs[] = {
  {"consolidate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 135,
    INT64_MIN, INT64_MAX, NULL},
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"file", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 136, INT64_MIN,
    INT64_MAX, NULL},
  {"force_stop", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 137,
    INT64_MIN, INT64_MAX, NULL},
  {"granularity", "int", NULL, "min=4KB,max=2GB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 138,
    4LL * WT_KILOBYTE, 2LL * WT_GIGABYTE, NULL},
  {"src_id", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 139, INT64_MIN,
    INT64_MAX, NULL},
  {"this_id", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 140, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_WT_SESSION_open_cursor_incremental_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 2, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
const char __WT_CONFIG_CHOICE_size[] = "size";

static const char *confchk_statistics2_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_cache_walk, __WT_CONFIG_CHOICE_fast, __WT_CONFIG_CHOICE_clear,
  __WT_CONFIG_CHOICE_size, __WT_CONFIG_CHOICE_tree_walk, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_open_cursor[] = {
  {"append", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 85, INT64_MIN,
    INT64_MAX, NULL},
  {"bulk", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 123, INT64_MIN,
    INT64_MAX, NULL},
  {"checkpoint", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 60, INT64_MIN,
    INT64_MAX, NULL},
  {"checkpoint_use_history", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    124, INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_wait", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 117,
    INT64_MIN, INT64_MAX, NULL},
  {"debug", "category", NULL, NULL, confchk_WT_SESSION_open_cursor_debug_subconfigs, 3,
    confchk_WT_SESSION_open_cursor_debug_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 125,
    INT64_MIN, INT64_MAX, NULL},
  {"dump", "string", NULL,
    "choices=[\"hex\",\"json\",\"pretty\",\"pretty_hex\","
    "\"print\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 133, INT64_MIN, INT64_MAX, confchk_dump_choices},
  {"incremental", "category", NULL, NULL, confchk_WT_SESSION_open_cursor_incremental_subconfigs, 7,
    confchk_WT_SESSION_open_cursor_incremental_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    134, INT64_MIN, INT64_MAX, NULL},
  {"next_random", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 141,
    INT64_MIN, INT64_MAX, NULL},
  {"next_random_sample_size", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    142, INT64_MIN, INT64_MAX, NULL},
  {"next_random_seed", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 143,
    INT64_MIN, INT64_MAX, NULL},
  {"overwrite", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 86,
    INT64_MIN, INT64_MAX, NULL},
  {"prefix_search", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 87,
    INT64_MIN, INT64_MAX, NULL},
  {"raw", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 144, INT64_MIN,
    INT64_MAX, NULL},
  {"read_once", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 145,
    INT64_MIN, INT64_MAX, NULL},
  {"readonly", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 67, INT64_MIN,
    INT64_MAX, NULL},
  {"skip_sort_check", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 146,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics", "list", NULL,
    "choices=[\"all\",\"cache_walk\",\"fast\",\"clear\","
    "\"size\",\"tree_walk\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 147, INT64_MIN, INT64_MAX,
    confchk_statistics2_choices},
  {"target", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 148, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_open_cursor_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 5, 7, 7,
  7, 7, 7, 8, 8, 8, 8, 8, 11, 12, 13, 13, 16, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19};

static const WT_CONFIG_CHECK confchk_WT_SESSION_prepare_transaction[] = {
  {"prepare_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 171,
    INT64_MIN, INT64_MAX, NULL},
  {"prepared_id", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 172,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_prepare_transaction_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_WT_SESSION_prepared_id_transaction[] = {
  {"prepared_id", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 172,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_prepared_id_transaction_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
const char __WT_CONFIG_CHOICE_commit[] = "commit";
const char __WT_CONFIG_CHOICE_first_commit[] = "first_commit";
const char __WT_CONFIG_CHOICE_prepare[] = "prepare";

static const char *confchk_get2_choices[] = {__WT_CONFIG_CHOICE_commit,
  __WT_CONFIG_CHOICE_first_commit, __WT_CONFIG_CHOICE_prepare, __WT_CONFIG_CHOICE_read, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_query_timestamp[] = {
  {"get", "string", NULL,
    "choices=[\"commit\",\"first_commit\",\"prepare\","
    "\"read\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 149, INT64_MIN, INT64_MAX, confchk_get2_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_query_timestamp_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const char *confchk_isolation3_choices[] = {__WT_CONFIG_CHOICE_read_uncommitted,
  __WT_CONFIG_CHOICE_read_committed, __WT_CONFIG_CHOICE_snapshot, NULL};

static const WT_CONFIG_CHECK confchk_WT_SESSION_reconfigure[] = {
  {"cache_cursors", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 296,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 203, 0,
    INT64_MAX, NULL},
  {"debug", "category", NULL, NULL, confchk_WT_CONNECTION_open_session_debug_subconfigs, 2,
    confchk_WT_CONNECTION_open_session_debug_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 125,
    INT64_MIN, INT64_MAX, NULL},
  {"ignore_cache_size", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 299,
    INT64_MIN, INT64_MAX, NULL},
  {"isolation", "string", NULL,
    "choices=[\"read-uncommitted\",\"read-committed\","
    "\"snapshot\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 164, INT64_MIN, INT64_MAX,
    confchk_isolation3_choices},
  {"prefetch", "category", NULL, NULL, confchk_WT_CONNECTION_open_session_prefetch_subconfigs, 1,
    confchk_WT_CONNECTION_open_session_prefetch_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    300, INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_reconfigure_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 3,
  3, 3, 3, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};

static const WT_CONFIG_CHECK confchk_WT_SESSION_rollback_transaction[] = {
  {"operation_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 166, 0,
    INT64_MAX, NULL},
  {"rollback_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 173,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_rollback_transaction_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_WT_SESSION_salvage[] = {
  {"force", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 118, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_salvage_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static const WT_CONFIG_CHECK confchk_WT_SESSION_timestamp_transaction[] = {
  {"commit_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 2,
    INT64_MIN, INT64_MAX, NULL},
  {"durable_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 3,
    INT64_MIN, INT64_MAX, NULL},
  {"prepare_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 171,
    INT64_MIN, INT64_MAX, NULL},
  {"read_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 4,
    INT64_MIN, INT64_MAX, NULL},
  {"rollback_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 173,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_timestamp_transaction_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_WT_SESSION_verify[] = {
  {"do_not_clear_txn_id", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    150, INT64_MIN, INT64_MAX, NULL},
  {"dump_address", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 151,
    INT64_MIN, INT64_MAX, NULL},
  {"dump_all_data", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 152,
    INT64_MIN, INT64_MAX, NULL},
  {"dump_blocks", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 153,
    INT64_MIN, INT64_MAX, NULL},
  {"dump_key_data", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 154,
    INT64_MIN, INT64_MAX, NULL},
  {"dump_layout", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 155,
    INT64_MIN, INT64_MAX, NULL},
  {"dump_offsets", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 156, INT64_MIN,
    INT64_MAX, NULL},
  {"dump_pages", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 157,
    INT64_MIN, INT64_MAX, NULL},
  {"dump_tree_shape", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 158,
    INT64_MIN, INT64_MAX, NULL},
  {"read_corrupt", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 159,
    INT64_MIN, INT64_MAX, NULL},
  {"stable_timestamp", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 160,
    INT64_MIN, INT64_MAX, NULL},
  {"strict", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 161, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_WT_SESSION_verify_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 9, 9, 9,
  9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12};

static const char *confchk_verbose4_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage3_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_colgroup_meta[] = {
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"source", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 8, INT64_MIN,
    INT64_MAX, NULL},
  {"type", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 9, INT64_MIN,
    INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose4_choices},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage3_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_colgroup_meta_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 6, 6, 7, 8, 8, 8, 8, 8, 8, 8, 8};

static const char *confchk_access_pattern_hint3_choices[] = {
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_random, __WT_CONFIG_CHOICE_sequential, NULL};

static const char *confchk_block_allocation2_choices[] = {
  __WT_CONFIG_CHOICE_best, __WT_CONFIG_CHOICE_first, NULL};

static const char *confchk_block_manager2_choices[] = {
  __WT_CONFIG_CHOICE_default, __WT_CONFIG_CHOICE_disagg, NULL};

static const char *confchk_checksum2_choices[] = {__WT_CONFIG_CHOICE_on, __WT_CONFIG_CHOICE_off,
  __WT_CONFIG_CHOICE_uncompressed, __WT_CONFIG_CHOICE_unencrypted, NULL};

static const char *confchk_format2_choices[] = {__WT_CONFIG_CHOICE_btree, NULL};

static const char *confchk_verbose5_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage4_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_file_config[] = {
  {"access_pattern_hint", "string", NULL, "choices=[\"none\",\"random\",\"sequential\"]", NULL, 0,
    NULL, WT_CONFIG_COMPILED_TYPE_STRING, 12, INT64_MIN, INT64_MAX,
    confchk_access_pattern_hint3_choices},
  {"allocation_size", "int", NULL, "min=512B,max=128MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    13, 512, 128LL * WT_MEGABYTE, NULL},
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"block_allocation", "string", NULL, "choices=[\"best\",\"first\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 14, INT64_MIN, INT64_MAX, confchk_block_allocation2_choices},
  {"block_compressor", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 15,
    INT64_MIN, INT64_MAX, NULL},
  {"block_manager", "string", NULL, "choices=[\"default\",\"disagg\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 16, INT64_MIN, INT64_MAX, confchk_block_manager2_choices},
  {"cache_resident", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 17,
    INT64_MIN, INT64_MAX, NULL},
  {"checksum", "string", NULL,
    "choices=[\"on\",\"off\",\"uncompressed\","
    "\"unencrypted\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 18, INT64_MIN, INT64_MAX,
    confchk_checksum2_choices},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"dictionary", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 19, 0, INT64_MAX,
    NULL},
  {"disaggregated", "category", NULL, NULL, confchk_WT_SESSION_create_disaggregated_subconfigs, 1,
    confchk_WT_SESSION_create_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_WT_SESSION_create_encryption_subconfigs, 2,
    confchk_WT_SESSION_create_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"format", "string", NULL, "choices=[\"btree\"]", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    25, INT64_MIN, INT64_MAX, confchk_format2_choices},
  {"huffman_key", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 26,
    INT64_MIN, INT64_MAX, NULL},
  {"huffman_value", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 27,
    INT64_MIN, INT64_MAX, NULL},
  {"ignore_in_memory_cache_size", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 28, INT64_MIN, INT64_MAX, NULL},
  {"in_memory", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 29,
    INT64_MIN, INT64_MAX, NULL},
  {"internal_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 30, 0,
    INT64_MAX, NULL},
  {"internal_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 31, 0,
    INT64_MAX, NULL},
  {"internal_key_truncate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    32, INT64_MIN, INT64_MAX, NULL},
  {"internal_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 33, 512, 512LL * WT_MEGABYTE, NULL},
  {"key_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_FORMAT,
    34, INT64_MIN, INT64_MAX, NULL},
  {"key_gap", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 35, 0, INT64_MAX,
    NULL},
  {"leaf_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 36, 0,
    INT64_MAX, NULL},
  {"leaf_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 37, 0,
    INT64_MAX, NULL},
  {"leaf_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    38, 512, 512LL * WT_MEGABYTE, NULL},
  {"leaf_value_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 39, 0,
    INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_WT_SESSION_create_log_subconfigs, 1,
    confchk_WT_SESSION_create_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"memory_page_image_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 42, 0,
    INT64_MAX, NULL},
  {"memory_page_max", "int", NULL, "min=512B,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    43, 512, 10LL * WT_TERABYTE, NULL},
  {"os_cache_dirty_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 44, 0,
    INT64_MAX, NULL},
  {"os_cache_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 45, 0,
    INT64_MAX, NULL},
  {"prefix_compression", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 46,
    INT64_MIN, INT64_MAX, NULL},
  {"prefix_compression_min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 47,
    0, INT64_MAX, NULL},
  {"split_deepen_min_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 48,
    INT64_MIN, INT64_MAX, NULL},
  {"split_deepen_per_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 49,
    INT64_MIN, INT64_MAX, NULL},
  {"split_pct", "int", NULL, "min=50,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 50, 50,
    100, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_WT_SESSION_create_tiered_storage_subconfigs, 8,
    confchk_WT_SESSION_create_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51,
    INT64_MIN, INT64_MAX, NULL},
  {"value_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_FORMAT, 59, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose5_choices},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage4_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_file_config_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 7, 11, 13, 14, 15, 15,
  17, 23, 23, 25, 30, 32, 32, 34, 36, 36, 36, 39, 40, 40, 42, 43, 43, 43, 43, 43, 43, 43, 43};

static const char *confchk_access_pattern_hint4_choices[] = {
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_random, __WT_CONFIG_CHOICE_sequential, NULL};

static const char *confchk_block_allocation3_choices[] = {
  __WT_CONFIG_CHOICE_best, __WT_CONFIG_CHOICE_first, NULL};

static const char *confchk_block_manager3_choices[] = {
  __WT_CONFIG_CHOICE_default, __WT_CONFIG_CHOICE_disagg, NULL};

static const char *confchk_checksum3_choices[] = {__WT_CONFIG_CHOICE_on, __WT_CONFIG_CHOICE_off,
  __WT_CONFIG_CHOICE_uncompressed, __WT_CONFIG_CHOICE_unencrypted, NULL};

static const char *confchk_format3_choices[] = {__WT_CONFIG_CHOICE_btree, NULL};

static const WT_CONFIG_CHECK confchk_live_restore_subconfigs[] = {
  {"bitmap", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 65, INT64_MIN,
    INT64_MAX, NULL},
  {"nbits", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 66, INT64_MIN, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_live_restore_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const char *confchk_verbose6_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage5_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_file_meta[] = {
  {"access_pattern_hint", "string", NULL, "choices=[\"none\",\"random\",\"sequential\"]", NULL, 0,
    NULL, WT_CONFIG_COMPILED_TYPE_STRING, 12, INT64_MIN, INT64_MAX,
    confchk_access_pattern_hint4_choices},
  {"allocation_size", "int", NULL, "min=512B,max=128MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    13, 512, 128LL * WT_MEGABYTE, NULL},
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"block_allocation", "string", NULL, "choices=[\"best\",\"first\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 14, INT64_MIN, INT64_MAX, confchk_block_allocation3_choices},
  {"block_compressor", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 15,
    INT64_MIN, INT64_MAX, NULL},
  {"block_manager", "string", NULL, "choices=[\"default\",\"disagg\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 16, INT64_MIN, INT64_MAX, confchk_block_manager3_choices},
  {"cache_resident", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 17,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 60, INT64_MIN,
    INT64_MAX, NULL},
  {"checkpoint_backup_info", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    61, INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_lsn", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 62,
    INT64_MIN, INT64_MAX, NULL},
  {"checksum", "string", NULL,
    "choices=[\"on\",\"off\",\"uncompressed\","
    "\"unencrypted\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 18, INT64_MIN, INT64_MAX,
    confchk_checksum3_choices},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"dictionary", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 19, 0, INT64_MAX,
    NULL},
  {"disaggregated", "category", NULL, NULL, confchk_WT_SESSION_create_disaggregated_subconfigs, 1,
    confchk_WT_SESSION_create_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_WT_SESSION_create_encryption_subconfigs, 2,
    confchk_WT_SESSION_create_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"format", "string", NULL, "choices=[\"btree\"]", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    25, INT64_MIN, INT64_MAX, confchk_format3_choices},
  {"huffman_key", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 26,
    INT64_MIN, INT64_MAX, NULL},
  {"huffman_value", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 27,
    INT64_MIN, INT64_MAX, NULL},
  {"id", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 63, INT64_MIN,
    INT64_MAX, NULL},
  {"ignore_in_memory_cache_size", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 28, INT64_MIN, INT64_MAX, NULL},
  {"in_memory", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 29,
    INT64_MIN, INT64_MAX, NULL},
  {"internal_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 30, 0,
    INT64_MAX, NULL},
  {"internal_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 31, 0,
    INT64_MAX, NULL},
  {"internal_key_truncate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    32, INT64_MIN, INT64_MAX, NULL},
  {"internal_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 33, 512, 512LL * WT_MEGABYTE, NULL},
  {"key_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_FORMAT,
    34, INT64_MIN, INT64_MAX, NULL},
  {"key_gap", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 35, 0, INT64_MAX,
    NULL},
  {"leaf_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 36, 0,
    INT64_MAX, NULL},
  {"leaf_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 37, 0,
    INT64_MAX, NULL},
  {"leaf_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    38, 512, 512LL * WT_MEGABYTE, NULL},
  {"leaf_value_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 39, 0,
    INT64_MAX, NULL},
  {"live_restore", "category", NULL, NULL, confchk_live_restore_subconfigs, 2,
    confchk_live_restore_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 64, INT64_MIN,
    INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_WT_SESSION_create_log_subconfigs, 1,
    confchk_WT_SESSION_create_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"memory_page_image_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 42, 0,
    INT64_MAX, NULL},
  {"memory_page_max", "int", NULL, "min=512B,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    43, 512, 10LL * WT_TERABYTE, NULL},
  {"os_cache_dirty_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 44, 0,
    INT64_MAX, NULL},
  {"os_cache_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 45, 0,
    INT64_MAX, NULL},
  {"prefix_compression", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 46,
    INT64_MIN, INT64_MAX, NULL},
  {"prefix_compression_min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 47,
    0, INT64_MAX, NULL},
  {"readonly", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 67, INT64_MIN,
    INT64_MAX, NULL},
  {"split_deepen_min_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 48,
    INT64_MIN, INT64_MAX, NULL},
  {"split_deepen_per_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 49,
    INT64_MIN, INT64_MAX, NULL},
  {"split_pct", "int", NULL, "min=50,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 50, 50,
    100, NULL},
  {"tiered_object", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 68,
    INT64_MIN, INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_WT_SESSION_create_tiered_storage_subconfigs, 8,
    confchk_WT_SESSION_create_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51,
    INT64_MIN, INT64_MAX, NULL},
  {"value_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_FORMAT, 59, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose6_choices},
  {"version", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 69, INT64_MIN,
    INT64_MAX, NULL},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage5_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_file_meta_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 7, 14, 16, 17, 18, 18, 20,
  27, 27, 29, 35, 37, 37, 39, 41, 41, 42, 45, 47, 47, 50, 51, 51, 51, 51, 51, 51, 51, 51};

static const char *confchk_verbose7_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage6_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_index_meta[] = {
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"extractor", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 70, INT64_MIN,
    INT64_MAX, NULL},
  {"immutable", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 71,
    INT64_MIN, INT64_MAX, NULL},
  {"key_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_FORMAT,
    34, INT64_MIN, INT64_MAX, NULL},
  {"source", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 8, INT64_MIN,
    INT64_MAX, NULL},
  {"type", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 9, INT64_MIN,
    INT64_MAX, NULL},
  {"value_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_FORMAT, 59, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose7_choices},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage6_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_index_meta_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 4, 4, 5, 5, 5, 5, 6,
  6, 7, 7, 7, 7, 7, 7, 7, 7, 8, 9, 9, 11, 12, 12, 12, 12, 12, 12, 12, 12};

static const char *confchk_local_files_action2_choices[] = {
  __WT_CONFIG_CHOICE_delete, __WT_CONFIG_CHOICE_fail, __WT_CONFIG_CHOICE_ignore, NULL};

static const char *confchk_role2_choices[] = {
  __WT_CONFIG_CHOICE_leader, __WT_CONFIG_CHOICE_follower, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_disaggregated_subconfigs[] = {
  {"checkpoint_meta", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 74,
    INT64_MIN, INT64_MAX, NULL},
  {"last_materialized_lsn", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 75,
    INT64_MIN, INT64_MAX, NULL},
  {"local_files_action", "string", NULL, "choices=[\"delete\",\"fail\",\"ignore\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 76, INT64_MIN, INT64_MAX, confchk_local_files_action2_choices},
  {"lose_all_my_data", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 77,
    INT64_MIN, INT64_MAX, NULL},
  {"page_log", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 21, INT64_MIN,
    INT64_MAX, NULL},
  {"role", "string", NULL, "choices=[\"leader\",\"follower\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 78, INT64_MIN, INT64_MAX, confchk_role2_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_disaggregated_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};

static const char *confchk_verbose8_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage7_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_layered_meta[] = {
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"disaggregated", "category", NULL, NULL, confchk_wiredtiger_open_disaggregated_subconfigs, 6,
    confchk_wiredtiger_open_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"ingest", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 79, INT64_MIN,
    INT64_MAX, NULL},
  {"key_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_FORMAT,
    34, INT64_MIN, INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_WT_SESSION_create_log_subconfigs, 1,
    confchk_WT_SESSION_create_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"stable", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 80, INT64_MIN,
    INT64_MAX, NULL},
  {"value_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_FORMAT, 59, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose8_choices},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage7_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_layered_meta_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 4, 5, 5, 5, 5, 5, 6,
  6, 7, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 11, 12, 12, 12, 12, 12, 12, 12, 12};

static const char *confchk_access_pattern_hint5_choices[] = {
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_random, __WT_CONFIG_CHOICE_sequential, NULL};

static const char *confchk_block_allocation4_choices[] = {
  __WT_CONFIG_CHOICE_best, __WT_CONFIG_CHOICE_first, NULL};

static const char *confchk_block_manager4_choices[] = {
  __WT_CONFIG_CHOICE_default, __WT_CONFIG_CHOICE_disagg, NULL};

static const char *confchk_checksum4_choices[] = {__WT_CONFIG_CHOICE_on, __WT_CONFIG_CHOICE_off,
  __WT_CONFIG_CHOICE_uncompressed, __WT_CONFIG_CHOICE_unencrypted, NULL};

static const char *confchk_format4_choices[] = {__WT_CONFIG_CHOICE_btree, NULL};

static const char *confchk_verbose9_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage8_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_object_meta[] = {
  {"access_pattern_hint", "string", NULL, "choices=[\"none\",\"random\",\"sequential\"]", NULL, 0,
    NULL, WT_CONFIG_COMPILED_TYPE_STRING, 12, INT64_MIN, INT64_MAX,
    confchk_access_pattern_hint5_choices},
  {"allocation_size", "int", NULL, "min=512B,max=128MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    13, 512, 128LL * WT_MEGABYTE, NULL},
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"block_allocation", "string", NULL, "choices=[\"best\",\"first\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 14, INT64_MIN, INT64_MAX, confchk_block_allocation4_choices},
  {"block_compressor", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 15,
    INT64_MIN, INT64_MAX, NULL},
  {"block_manager", "string", NULL, "choices=[\"default\",\"disagg\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 16, INT64_MIN, INT64_MAX, confchk_block_manager4_choices},
  {"cache_resident", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 17,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 60, INT64_MIN,
    INT64_MAX, NULL},
  {"checkpoint_backup_info", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    61, INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_lsn", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 62,
    INT64_MIN, INT64_MAX, NULL},
  {"checksum", "string", NULL,
    "choices=[\"on\",\"off\",\"uncompressed\","
    "\"unencrypted\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 18, INT64_MIN, INT64_MAX,
    confchk_checksum4_choices},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"dictionary", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 19, 0, INT64_MAX,
    NULL},
  {"disaggregated", "category", NULL, NULL, confchk_WT_SESSION_create_disaggregated_subconfigs, 1,
    confchk_WT_SESSION_create_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_WT_SESSION_create_encryption_subconfigs, 2,
    confchk_WT_SESSION_create_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"flush_time", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 72, INT64_MIN,
    INT64_MAX, NULL},
  {"flush_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 73,
    INT64_MIN, INT64_MAX, NULL},
  {"format", "string", NULL, "choices=[\"btree\"]", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    25, INT64_MIN, INT64_MAX, confchk_format4_choices},
  {"huffman_key", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 26,
    INT64_MIN, INT64_MAX, NULL},
  {"huffman_value", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 27,
    INT64_MIN, INT64_MAX, NULL},
  {"id", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 63, INT64_MIN,
    INT64_MAX, NULL},
  {"ignore_in_memory_cache_size", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 28, INT64_MIN, INT64_MAX, NULL},
  {"in_memory", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 29,
    INT64_MIN, INT64_MAX, NULL},
  {"internal_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 30, 0,
    INT64_MAX, NULL},
  {"internal_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 31, 0,
    INT64_MAX, NULL},
  {"internal_key_truncate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    32, INT64_MIN, INT64_MAX, NULL},
  {"internal_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 33, 512, 512LL * WT_MEGABYTE, NULL},
  {"key_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_FORMAT,
    34, INT64_MIN, INT64_MAX, NULL},
  {"key_gap", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 35, 0, INT64_MAX,
    NULL},
  {"leaf_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 36, 0,
    INT64_MAX, NULL},
  {"leaf_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 37, 0,
    INT64_MAX, NULL},
  {"leaf_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    38, 512, 512LL * WT_MEGABYTE, NULL},
  {"leaf_value_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 39, 0,
    INT64_MAX, NULL},
  {"live_restore", "category", NULL, NULL, confchk_live_restore_subconfigs, 2,
    confchk_live_restore_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 64, INT64_MIN,
    INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_WT_SESSION_create_log_subconfigs, 1,
    confchk_WT_SESSION_create_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"memory_page_image_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 42, 0,
    INT64_MAX, NULL},
  {"memory_page_max", "int", NULL, "min=512B,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    43, 512, 10LL * WT_TERABYTE, NULL},
  {"os_cache_dirty_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 44, 0,
    INT64_MAX, NULL},
  {"os_cache_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 45, 0,
    INT64_MAX, NULL},
  {"prefix_compression", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 46,
    INT64_MIN, INT64_MAX, NULL},
  {"prefix_compression_min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 47,
    0, INT64_MAX, NULL},
  {"readonly", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 67, INT64_MIN,
    INT64_MAX, NULL},
  {"split_deepen_min_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 48,
    INT64_MIN, INT64_MAX, NULL},
  {"split_deepen_per_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 49,
    INT64_MIN, INT64_MAX, NULL},
  {"split_pct", "int", NULL, "min=50,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 50, 50,
    100, NULL},
  {"tiered_object", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 68,
    INT64_MIN, INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_WT_SESSION_create_tiered_storage_subconfigs, 8,
    confchk_WT_SESSION_create_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51,
    INT64_MIN, INT64_MAX, NULL},
  {"value_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_FORMAT, 59, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose9_choices},
  {"version", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 69, INT64_MIN,
    INT64_MAX, NULL},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage8_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_object_meta_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 7, 14, 16, 17, 20, 20,
  22, 29, 29, 31, 37, 39, 39, 41, 43, 43, 44, 47, 49, 49, 52, 53, 53, 53, 53, 53, 53, 53, 53};

static const char *confchk_verbose10_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage9_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_table_meta[] = {
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"colgroups", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 81, INT64_MIN,
    INT64_MAX, NULL},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"key_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_FORMAT,
    34, INT64_MIN, INT64_MAX, NULL},
  {"value_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_FORMAT, 59, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose10_choices},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage9_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_table_meta_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 5, 5, 5, 5, 5, 5, 5,
  5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 8, 9, 9, 9, 9, 9, 9, 9, 9};

static const char *confchk_access_pattern_hint6_choices[] = {
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_random, __WT_CONFIG_CHOICE_sequential, NULL};

static const char *confchk_block_allocation5_choices[] = {
  __WT_CONFIG_CHOICE_best, __WT_CONFIG_CHOICE_first, NULL};

static const char *confchk_block_manager5_choices[] = {
  __WT_CONFIG_CHOICE_default, __WT_CONFIG_CHOICE_disagg, NULL};

static const char *confchk_checksum5_choices[] = {__WT_CONFIG_CHOICE_on, __WT_CONFIG_CHOICE_off,
  __WT_CONFIG_CHOICE_uncompressed, __WT_CONFIG_CHOICE_unencrypted, NULL};

static const char *confchk_format5_choices[] = {__WT_CONFIG_CHOICE_btree, NULL};

static const char *confchk_verbose11_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage10_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_tier_meta[] = {
  {"access_pattern_hint", "string", NULL, "choices=[\"none\",\"random\",\"sequential\"]", NULL, 0,
    NULL, WT_CONFIG_COMPILED_TYPE_STRING, 12, INT64_MIN, INT64_MAX,
    confchk_access_pattern_hint6_choices},
  {"allocation_size", "int", NULL, "min=512B,max=128MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    13, 512, 128LL * WT_MEGABYTE, NULL},
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"block_allocation", "string", NULL, "choices=[\"best\",\"first\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 14, INT64_MIN, INT64_MAX, confchk_block_allocation5_choices},
  {"block_compressor", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 15,
    INT64_MIN, INT64_MAX, NULL},
  {"block_manager", "string", NULL, "choices=[\"default\",\"disagg\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 16, INT64_MIN, INT64_MAX, confchk_block_manager5_choices},
  {"bucket", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 53, INT64_MIN,
    INT64_MAX, NULL},
  {"bucket_prefix", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 54,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_directory", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 55,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_resident", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 17,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 60, INT64_MIN,
    INT64_MAX, NULL},
  {"checkpoint_backup_info", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    61, INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_lsn", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 62,
    INT64_MIN, INT64_MAX, NULL},
  {"checksum", "string", NULL,
    "choices=[\"on\",\"off\",\"uncompressed\","
    "\"unencrypted\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 18, INT64_MIN, INT64_MAX,
    confchk_checksum5_choices},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"dictionary", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 19, 0, INT64_MAX,
    NULL},
  {"disaggregated", "category", NULL, NULL, confchk_WT_SESSION_create_disaggregated_subconfigs, 1,
    confchk_WT_SESSION_create_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_WT_SESSION_create_encryption_subconfigs, 2,
    confchk_WT_SESSION_create_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"format", "string", NULL, "choices=[\"btree\"]", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    25, INT64_MIN, INT64_MAX, confchk_format5_choices},
  {"huffman_key", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 26,
    INT64_MIN, INT64_MAX, NULL},
  {"huffman_value", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 27,
    INT64_MIN, INT64_MAX, NULL},
  {"id", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 63, INT64_MIN,
    INT64_MAX, NULL},
  {"ignore_in_memory_cache_size", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 28, INT64_MIN, INT64_MAX, NULL},
  {"in_memory", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 29,
    INT64_MIN, INT64_MAX, NULL},
  {"internal_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 30, 0,
    INT64_MAX, NULL},
  {"internal_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 31, 0,
    INT64_MAX, NULL},
  {"internal_key_truncate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    32, INT64_MIN, INT64_MAX, NULL},
  {"internal_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 33, 512, 512LL * WT_MEGABYTE, NULL},
  {"key_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_FORMAT,
    34, INT64_MIN, INT64_MAX, NULL},
  {"key_gap", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 35, 0, INT64_MAX,
    NULL},
  {"leaf_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 36, 0,
    INT64_MAX, NULL},
  {"leaf_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 37, 0,
    INT64_MAX, NULL},
  {"leaf_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    38, 512, 512LL * WT_MEGABYTE, NULL},
  {"leaf_value_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 39, 0,
    INT64_MAX, NULL},
  {"live_restore", "category", NULL, NULL, confchk_live_restore_subconfigs, 2,
    confchk_live_restore_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 64, INT64_MIN,
    INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_WT_SESSION_create_log_subconfigs, 1,
    confchk_WT_SESSION_create_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"memory_page_image_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 42, 0,
    INT64_MAX, NULL},
  {"memory_page_max", "int", NULL, "min=512B,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    43, 512, 10LL * WT_TERABYTE, NULL},
  {"os_cache_dirty_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 44, 0,
    INT64_MAX, NULL},
  {"os_cache_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 45, 0,
    INT64_MAX, NULL},
  {"prefix_compression", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 46,
    INT64_MIN, INT64_MAX, NULL},
  {"prefix_compression_min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 47,
    0, INT64_MAX, NULL},
  {"readonly", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 67, INT64_MIN,
    INT64_MAX, NULL},
  {"split_deepen_min_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 48,
    INT64_MIN, INT64_MAX, NULL},
  {"split_deepen_per_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 49,
    INT64_MIN, INT64_MAX, NULL},
  {"split_pct", "int", NULL, "min=50,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 50, 50,
    100, NULL},
  {"tiered_object", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 68,
    INT64_MIN, INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_WT_SESSION_create_tiered_storage_subconfigs, 8,
    confchk_WT_SESSION_create_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51,
    INT64_MIN, INT64_MAX, NULL},
  {"value_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_FORMAT, 59, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose11_choices},
  {"version", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 69, INT64_MIN,
    INT64_MAX, NULL},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage10_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_tier_meta_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 9, 17, 19, 20, 21, 21, 23,
  30, 30, 32, 38, 40, 40, 42, 44, 44, 45, 48, 50, 50, 53, 54, 54, 54, 54, 54, 54, 54, 54};

static const char *confchk_access_pattern_hint7_choices[] = {
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_random, __WT_CONFIG_CHOICE_sequential, NULL};

static const char *confchk_block_allocation6_choices[] = {
  __WT_CONFIG_CHOICE_best, __WT_CONFIG_CHOICE_first, NULL};

static const char *confchk_block_manager6_choices[] = {
  __WT_CONFIG_CHOICE_default, __WT_CONFIG_CHOICE_disagg, NULL};

static const char *confchk_checksum6_choices[] = {__WT_CONFIG_CHOICE_on, __WT_CONFIG_CHOICE_off,
  __WT_CONFIG_CHOICE_uncompressed, __WT_CONFIG_CHOICE_unencrypted, NULL};

static const char *confchk_format6_choices[] = {__WT_CONFIG_CHOICE_btree, NULL};

static const char *confchk_verbose12_choices[] = {__WT_CONFIG_CHOICE_write_timestamp, NULL};

static const char *confchk_write_timestamp_usage11_choices[] = {__WT_CONFIG_CHOICE_always,
  __WT_CONFIG_CHOICE_key_consistent, __WT_CONFIG_CHOICE_mixed_mode, __WT_CONFIG_CHOICE_never,
  __WT_CONFIG_CHOICE_none, __WT_CONFIG_CHOICE_ordered, NULL};

static const WT_CONFIG_CHECK confchk_tiered_meta[] = {
  {"access_pattern_hint", "string", NULL, "choices=[\"none\",\"random\",\"sequential\"]", NULL, 0,
    NULL, WT_CONFIG_COMPILED_TYPE_STRING, 12, INT64_MIN, INT64_MAX,
    confchk_access_pattern_hint7_choices},
  {"allocation_size", "int", NULL, "min=512B,max=128MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    13, 512, 128LL * WT_MEGABYTE, NULL},
  {"app_metadata", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 0,
    INT64_MIN, INT64_MAX, NULL},
  {"assert", "category", NULL, NULL, confchk_WT_SESSION_create_assert_subconfigs, 4,
    confchk_WT_SESSION_create_assert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 1,
    INT64_MIN, INT64_MAX, NULL},
  {"block_allocation", "string", NULL, "choices=[\"best\",\"first\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 14, INT64_MIN, INT64_MAX, confchk_block_allocation6_choices},
  {"block_compressor", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 15,
    INT64_MIN, INT64_MAX, NULL},
  {"block_manager", "string", NULL, "choices=[\"default\",\"disagg\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 16, INT64_MIN, INT64_MAX, confchk_block_manager6_choices},
  {"cache_resident", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 17,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 60, INT64_MIN,
    INT64_MAX, NULL},
  {"checkpoint_backup_info", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    61, INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_lsn", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 62,
    INT64_MIN, INT64_MAX, NULL},
  {"checksum", "string", NULL,
    "choices=[\"on\",\"off\",\"uncompressed\","
    "\"unencrypted\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 18, INT64_MIN, INT64_MAX,
    confchk_checksum6_choices},
  {"collator", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 6, INT64_MIN,
    INT64_MAX, NULL},
  {"columns", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 7, INT64_MIN,
    INT64_MAX, NULL},
  {"dictionary", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 19, 0, INT64_MAX,
    NULL},
  {"disaggregated", "category", NULL, NULL, confchk_WT_SESSION_create_disaggregated_subconfigs, 1,
    confchk_WT_SESSION_create_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_WT_SESSION_create_encryption_subconfigs, 2,
    confchk_WT_SESSION_create_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"flush_time", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 72, INT64_MIN,
    INT64_MAX, NULL},
  {"flush_timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 73,
    INT64_MIN, INT64_MAX, NULL},
  {"format", "string", NULL, "choices=[\"btree\"]", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    25, INT64_MIN, INT64_MAX, confchk_format6_choices},
  {"huffman_key", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 26,
    INT64_MIN, INT64_MAX, NULL},
  {"huffman_value", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 27,
    INT64_MIN, INT64_MAX, NULL},
  {"id", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 63, INT64_MIN,
    INT64_MAX, NULL},
  {"ignore_in_memory_cache_size", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 28, INT64_MIN, INT64_MAX, NULL},
  {"in_memory", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 29,
    INT64_MIN, INT64_MAX, NULL},
  {"internal_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 30, 0,
    INT64_MAX, NULL},
  {"internal_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 31, 0,
    INT64_MAX, NULL},
  {"internal_key_truncate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    32, INT64_MIN, INT64_MAX, NULL},
  {"internal_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 33, 512, 512LL * WT_MEGABYTE, NULL},
  {"key_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_FORMAT,
    34, INT64_MIN, INT64_MAX, NULL},
  {"key_gap", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 35, 0, INT64_MAX,
    NULL},
  {"last", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 82, INT64_MIN,
    INT64_MAX, NULL},
  {"leaf_item_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 36, 0,
    INT64_MAX, NULL},
  {"leaf_key_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 37, 0,
    INT64_MAX, NULL},
  {"leaf_page_max", "int", NULL, "min=512B,max=512MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    38, 512, 512LL * WT_MEGABYTE, NULL},
  {"leaf_value_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 39, 0,
    INT64_MAX, NULL},
  {"live_restore", "category", NULL, NULL, confchk_live_restore_subconfigs, 2,
    confchk_live_restore_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 64, INT64_MIN,
    INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_WT_SESSION_create_log_subconfigs, 1,
    confchk_WT_SESSION_create_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"memory_page_image_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 42, 0,
    INT64_MAX, NULL},
  {"memory_page_max", "int", NULL, "min=512B,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    43, 512, 10LL * WT_TERABYTE, NULL},
  {"oldest", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 83, INT64_MIN,
    INT64_MAX, NULL},
  {"os_cache_dirty_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 44, 0,
    INT64_MAX, NULL},
  {"os_cache_max", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 45, 0,
    INT64_MAX, NULL},
  {"prefix_compression", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 46,
    INT64_MIN, INT64_MAX, NULL},
  {"prefix_compression_min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 47,
    0, INT64_MAX, NULL},
  {"readonly", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 67, INT64_MIN,
    INT64_MAX, NULL},
  {"split_deepen_min_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 48,
    INT64_MIN, INT64_MAX, NULL},
  {"split_deepen_per_child", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 49,
    INT64_MIN, INT64_MAX, NULL},
  {"split_pct", "int", NULL, "min=50,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 50, 50,
    100, NULL},
  {"tiered_object", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 68,
    INT64_MIN, INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_WT_SESSION_create_tiered_storage_subconfigs, 8,
    confchk_WT_SESSION_create_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51,
    INT64_MIN, INT64_MAX, NULL},
  {"tiers", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 84, INT64_MIN,
    INT64_MAX, NULL},
  {"value_format", "format", __wt_struct_confchk, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_FORMAT, 59, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL, "choices=[\"write_timestamp\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX, confchk_verbose12_choices},
  {"version", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 69, INT64_MIN,
    INT64_MAX, NULL},
  {"write_timestamp_usage", "string", NULL,
    "choices=[\"always\",\"key_consistent\",\"mixed_mode\","
    "\"never\",\"none\",\"ordered\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 11, INT64_MIN, INT64_MAX,
    confchk_write_timestamp_usage11_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_tiered_meta_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 7, 14, 16, 17, 20, 20,
  22, 29, 29, 31, 38, 40, 40, 43, 45, 45, 46, 49, 52, 52, 55, 56, 56, 56, 56, 56, 56, 56, 56};
const char __WT_CONFIG_CHOICE_FILE[] = "FILE";
const char __WT_CONFIG_CHOICE_DRAM[] = "DRAM";

static const char *confchk_type_choices[] = {
  __WT_CONFIG_CHOICE_FILE, __WT_CONFIG_CHOICE_DRAM, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_chunk_cache_subconfigs[] = {
  {"capacity", "int", NULL, "min=512KB,max=100TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 306,
    512LL * WT_KILOBYTE, 100LL * WT_TERABYTE, NULL},
  {"chunk_cache_evict_trigger", "int", NULL, "min=0,max=100", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 307, 0, 100, NULL},
  {"chunk_size", "int", NULL, "min=512KB,max=100GB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    114, 512LL * WT_KILOBYTE, 100LL * WT_GIGABYTE, NULL},
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"flushed_data_cache_insertion", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, 309, INT64_MIN, INT64_MAX, NULL},
  {"hashsize", "int", NULL, "min=64,max=1048576", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 195,
    64, 1048576LL, NULL},
  {"pinned", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 214, INT64_MIN,
    INT64_MAX, NULL},
  {"storage_path", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 308,
    INT64_MIN, INT64_MAX, NULL},
  {"type", "string", NULL, "choices=[\"FILE\",\"DRAM\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 9, INT64_MIN, INT64_MAX, confchk_type_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_chunk_cache_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3,
    3, 4, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_compatibility_subconfigs[] = {
  {"release", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 216, INT64_MIN,
    INT64_MAX, NULL},
  {"require_max", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 310,
    INT64_MIN, INT64_MAX, NULL},
  {"require_min", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 311,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_compatibility_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_encryption_subconfigs[] = {
  {"keyid", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 24, INT64_MIN,
    INT64_MAX, NULL},
  {"name", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 23, INT64_MIN,
    INT64_MAX, NULL},
  {"secretkey", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 316, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_encryption_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] =
  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

static const char *confchk_extra_diagnostics2_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_checkpoint_validate, __WT_CONFIG_CHOICE_cursor_check,
  __WT_CONFIG_CHOICE_disk_validate, __WT_CONFIG_CHOICE_eviction_check,
  __WT_CONFIG_CHOICE_generation_check, __WT_CONFIG_CHOICE_hs_validate,
  __WT_CONFIG_CHOICE_key_out_of_order, __WT_CONFIG_CHOICE_log_validate, __WT_CONFIG_CHOICE_prepared,
  __WT_CONFIG_CHOICE_slow_operation, __WT_CONFIG_CHOICE_txn_visibility, NULL};
const char __WT_CONFIG_CHOICE_data[] = "data";

static const char *confchk_file_extend_choices[] = {
  __WT_CONFIG_CHOICE_data, __WT_CONFIG_CHOICE_log, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_hash_subconfigs[] = {
  {"buckets", "int", NULL, "min=64,max=65536", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 320, 64,
    65536, NULL},
  {"dhandle_buckets", "int", NULL, "min=64,max=65536", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    321, 64, 65536, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_hash_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
  1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const char *confchk_json_output2_choices[] = {
  __WT_CONFIG_CHOICE_error, __WT_CONFIG_CHOICE_message, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_live_restore_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"path", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 273, INT64_MIN,
    INT64_MAX, NULL},
  {"read_size", "int", NULL, "min=512B,max=16MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 323,
    512, 16LL * WT_MEGABYTE, NULL},
  {"threads_max", "int", NULL, "min=0,max=12", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 238, 0,
    12, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_live_restore_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

static const char *confchk_recover_choices[] = {
  __WT_CONFIG_CHOICE_error, __WT_CONFIG_CHOICE_on, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_log_subconfigs[] = {
  {"archive", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 266, INT64_MIN,
    INT64_MAX, NULL},
  {"compressor", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 324,
    INT64_MIN, INT64_MAX, NULL},
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"file_max", "int", NULL, "min=100KB,max=2GB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 261,
    100LL * WT_KILOBYTE, 2LL * WT_GIGABYTE, NULL},
  {"force_write_wait", "int", NULL, "min=1,max=60", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 325,
    1, 60, NULL},
  {"os_cache_dirty_pct", "int", NULL, "min=0,max=100", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    267, 0, 100, NULL},
  {"path", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 273, INT64_MIN,
    INT64_MAX, NULL},
  {"prealloc", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 268,
    INT64_MIN, INT64_MAX, NULL},
  {"prealloc_init_count", "int", NULL, "min=1,max=500", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    269, 1, 500, NULL},
  {"recover", "string", NULL, "choices=[\"error\",\"on\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 326, INT64_MIN, INT64_MAX, confchk_recover_choices},
  {"remove", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 270, INT64_MIN,
    INT64_MAX, NULL},
  {"zero_fill", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 271,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_log_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2,
  2, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 9, 9, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_page_delta_subconfigs[] = {
  {"delta_pct", "int", NULL, "min=1,max=1000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 275, 1,
    1000, NULL},
  {"flatten_leaf_page_delta", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    276, INT64_MIN, INT64_MAX, NULL},
  {"internal_page_delta", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    277, INT64_MIN, INT64_MAX, NULL},
  {"leaf_page_delta", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 278,
    INT64_MIN, INT64_MAX, NULL},
  {"max_consecutive_delta", "int", NULL, "min=1,max=32", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    279, 1, 32, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_page_delta_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] =
  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_prefetch_subconfigs[] = {
  {"available", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 331,
    INT64_MIN, INT64_MAX, NULL},
  {"default", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 332, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_prefetch_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
  1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const char *confchk_statistics3_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_cache_walk, __WT_CONFIG_CHOICE_fast, __WT_CONFIG_CHOICE_none,
  __WT_CONFIG_CHOICE_clear, __WT_CONFIG_CHOICE_tree_walk, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_statistics_log_subconfigs[] = {
  {"json", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 287, INT64_MIN,
    INT64_MAX, NULL},
  {"on_close", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 288,
    INT64_MIN, INT64_MAX, NULL},
  {"path", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 273, INT64_MIN,
    INT64_MAX, NULL},
  {"sources", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 289, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 290, INT64_MIN,
    INT64_MAX, NULL},
  {"wait", "int", NULL, "min=0,max=100000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 209, 0,
    100000, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_statistics_log_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 3, 3, 3, 4, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6};

static const WT_CONFIG_CHECK confchk_tiered_storage_subconfigs[] = {
  {"auth_token", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 52, INT64_MIN,
    INT64_MAX, NULL},
  {"bucket", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 53, INT64_MIN,
    INT64_MAX, NULL},
  {"bucket_prefix", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 54,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_directory", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 55,
    INT64_MIN, INT64_MAX, NULL},
  {"interval", "int", NULL, "min=1,max=1000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 338, 1,
    1000, NULL},
  {"local_retention", "int", NULL, "min=0,max=10000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    56, 0, 10000, NULL},
  {"name", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 23, INT64_MIN,
    INT64_MAX, NULL},
  {"shared", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 58, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_tiered_storage_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 4, 4,
  4, 4, 4, 4, 5, 5, 5, 6, 6, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};

static const char *confchk_timing_stress_for_test2_choices[] = {
  __WT_CONFIG_CHOICE_aggressive_stash_free, __WT_CONFIG_CHOICE_aggressive_sweep,
  __WT_CONFIG_CHOICE_backup_rename, __WT_CONFIG_CHOICE_checkpoint_evict_page,
  __WT_CONFIG_CHOICE_checkpoint_handle, __WT_CONFIG_CHOICE_checkpoint_slow,
  __WT_CONFIG_CHOICE_checkpoint_stop, __WT_CONFIG_CHOICE_commit_transaction_slow,
  __WT_CONFIG_CHOICE_compact_slow, __WT_CONFIG_CHOICE_conn_close_stress_log_printf,
  __WT_CONFIG_CHOICE_evict_reposition, __WT_CONFIG_CHOICE_failpoint_eviction_split,
  __WT_CONFIG_CHOICE_failpoint_history_store_delete_key_from_ts,
  __WT_CONFIG_CHOICE_history_store_checkpoint_delay, __WT_CONFIG_CHOICE_history_store_search,
  __WT_CONFIG_CHOICE_history_store_sweep_race, __WT_CONFIG_CHOICE_live_restore_clean_up,
  __WT_CONFIG_CHOICE_open_index_slow, __WT_CONFIG_CHOICE_prefetch_1, __WT_CONFIG_CHOICE_prefetch_2,
  __WT_CONFIG_CHOICE_prefetch_3, __WT_CONFIG_CHOICE_prefix_compare,
  __WT_CONFIG_CHOICE_prepare_checkpoint_delay, __WT_CONFIG_CHOICE_prepare_resolution_1,
  __WT_CONFIG_CHOICE_prepare_resolution_2, __WT_CONFIG_CHOICE_session_alter_slow,
  __WT_CONFIG_CHOICE_sleep_before_read_overflow_onpage, __WT_CONFIG_CHOICE_split_1,
  __WT_CONFIG_CHOICE_split_2, __WT_CONFIG_CHOICE_split_3, __WT_CONFIG_CHOICE_split_4,
  __WT_CONFIG_CHOICE_split_5, __WT_CONFIG_CHOICE_split_6, __WT_CONFIG_CHOICE_split_7,
  __WT_CONFIG_CHOICE_split_8, __WT_CONFIG_CHOICE_tiered_flush_finish, NULL};
const char __WT_CONFIG_CHOICE_dsync[] = "dsync";
const char __WT_CONFIG_CHOICE_fsync[] = "fsync";

static const char *confchk_method2_choices[] = {
  __WT_CONFIG_CHOICE_dsync, __WT_CONFIG_CHOICE_fsync, __WT_CONFIG_CHOICE_none, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_transaction_sync_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 41, INT64_MIN,
    INT64_MAX, NULL},
  {"method", "string", NULL, "choices=[\"dsync\",\"fsync\",\"none\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_STRING, 211, INT64_MIN, INT64_MAX, confchk_method2_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t
  confchk_wiredtiger_open_transaction_sync_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const char *confchk_verbose13_choices[] = {__WT_CONFIG_CHOICE_all, __WT_CONFIG_CHOICE_api,
  __WT_CONFIG_CHOICE_backup, __WT_CONFIG_CHOICE_block, __WT_CONFIG_CHOICE_block_cache,
  __WT_CONFIG_CHOICE_checkpoint, __WT_CONFIG_CHOICE_checkpoint_cleanup,
  __WT_CONFIG_CHOICE_checkpoint_progress, __WT_CONFIG_CHOICE_chunkcache, __WT_CONFIG_CHOICE_compact,
  __WT_CONFIG_CHOICE_compact_progress, __WT_CONFIG_CHOICE_configuration,
  __WT_CONFIG_CHOICE_disaggregated_storage, __WT_CONFIG_CHOICE_error_returns,
  __WT_CONFIG_CHOICE_eviction, __WT_CONFIG_CHOICE_fileops, __WT_CONFIG_CHOICE_generation,
  __WT_CONFIG_CHOICE_handleops, __WT_CONFIG_CHOICE_history_store,
  __WT_CONFIG_CHOICE_history_store_activity, __WT_CONFIG_CHOICE_layered,
  __WT_CONFIG_CHOICE_live_restore, __WT_CONFIG_CHOICE_live_restore_progress, __WT_CONFIG_CHOICE_log,
  __WT_CONFIG_CHOICE_metadata, __WT_CONFIG_CHOICE_mutex, __WT_CONFIG_CHOICE_out_of_order,
  __WT_CONFIG_CHOICE_overflow, __WT_CONFIG_CHOICE_page_delta, __WT_CONFIG_CHOICE_prefetch,
  __WT_CONFIG_CHOICE_read, __WT_CONFIG_CHOICE_reconcile, __WT_CONFIG_CHOICE_recovery,
  __WT_CONFIG_CHOICE_recovery_progress, __WT_CONFIG_CHOICE_rts, __WT_CONFIG_CHOICE_salvage,
  __WT_CONFIG_CHOICE_shared_cache, __WT_CONFIG_CHOICE_split, __WT_CONFIG_CHOICE_sweep,
  __WT_CONFIG_CHOICE_temporary, __WT_CONFIG_CHOICE_thread_group, __WT_CONFIG_CHOICE_tiered,
  __WT_CONFIG_CHOICE_timestamp, __WT_CONFIG_CHOICE_transaction, __WT_CONFIG_CHOICE_verify,
  __WT_CONFIG_CHOICE_version, __WT_CONFIG_CHOICE_write, NULL};

static const char *confchk_write_through_choices[] = {
  __WT_CONFIG_CHOICE_data, __WT_CONFIG_CHOICE_log, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open[] = {
  {"backup_restore_target", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 302,
    INT64_MIN, INT64_MAX, NULL},
  {"block_cache", "category", NULL, NULL, confchk_wiredtiger_open_block_cache_subconfigs, 12,
    confchk_wiredtiger_open_block_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 189,
    INT64_MIN, INT64_MAX, NULL},
  {"buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    303, -1, 1LL * WT_MEGABYTE, NULL},
  {"builtin_extension_config", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    304, INT64_MIN, INT64_MAX, NULL},
  {"cache_cursors", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 296,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_eviction_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs, 2,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 200, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 203, 0,
    INT64_MAX, NULL},
  {"cache_overhead", "int", NULL, "min=0,max=30", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 204,
    0, 30, NULL},
  {"cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 205,
    1LL * WT_MEGABYTE, 10LL * WT_TERABYTE, NULL},
  {"cache_stuck_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 206,
    0, INT64_MAX, NULL},
  {"checkpoint", "category", NULL, NULL, confchk_wiredtiger_open_checkpoint_subconfigs, 2,
    confchk_wiredtiger_open_checkpoint_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 207,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_cleanup", "category", NULL, NULL,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs, 3,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    210, INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_sync", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 305,
    INT64_MIN, INT64_MAX, NULL},
  {"chunk_cache", "category", NULL, NULL, confchk_wiredtiger_open_chunk_cache_subconfigs, 9,
    confchk_wiredtiger_open_chunk_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 213,
    INT64_MIN, INT64_MAX, NULL},
  {"compatibility", "category", NULL, NULL, confchk_wiredtiger_open_compatibility_subconfigs, 3,
    confchk_wiredtiger_open_compatibility_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 215,
    INT64_MIN, INT64_MAX, NULL},
  {"compile_configuration_count", "int", NULL, "min=500", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 312, 500, INT64_MAX, NULL},
  {"config_base", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 313,
    INT64_MIN, INT64_MAX, NULL},
  {"create", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 314, INT64_MIN,
    INT64_MAX, NULL},
  {"debug_mode", "category", NULL, NULL, confchk_wiredtiger_open_debug_mode_subconfigs, 18,
    confchk_wiredtiger_open_debug_mode_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 217,
    INT64_MIN, INT64_MAX, NULL},
  {"direct_io", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 315, INT64_MIN,
    INT64_MAX, NULL},
  {"disaggregated", "category", NULL, NULL, confchk_wiredtiger_open_disaggregated_subconfigs, 6,
    confchk_wiredtiger_open_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_wiredtiger_open_encryption_subconfigs, 3,
    confchk_wiredtiger_open_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"error_prefix", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 236,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction", "category", NULL, NULL, confchk_wiredtiger_open_eviction_subconfigs, 5,
    confchk_wiredtiger_open_eviction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 237,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction_checkpoint_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 243, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_target", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 244, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_trigger", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 245, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    246, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_trigger", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    247, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 248, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_trigger", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 249, 0, 10LL * WT_TERABYTE, NULL},
  {"exclusive", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 98,
    INT64_MIN, INT64_MAX, NULL},
  {"extensions", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 317, INT64_MIN,
    INT64_MAX, NULL},
  {"extra_diagnostics", "list", NULL,
    "choices=[\"all\",\"checkpoint_validate\",\"cursor_check\""
    ",\"disk_validate\",\"eviction_check\",\"generation_check\","
    "\"hs_validate\",\"key_out_of_order\",\"log_validate\","
    "\"prepared\",\"slow_operation\",\"txn_visibility\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 250, INT64_MIN, INT64_MAX,
    confchk_extra_diagnostics2_choices},
  {"file_extend", "list", NULL, "choices=[\"data\",\"log\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 318, INT64_MIN, INT64_MAX, confchk_file_extend_choices},
  {"file_manager", "category", NULL, NULL, confchk_wiredtiger_open_file_manager_subconfigs, 3,
    confchk_wiredtiger_open_file_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 251,
    INT64_MIN, INT64_MAX, NULL},
  {"generation_drain_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    255, 0, INT64_MAX, NULL},
  {"hash", "category", NULL, NULL, confchk_wiredtiger_open_hash_subconfigs, 2,
    confchk_wiredtiger_open_hash_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 319, INT64_MIN,
    INT64_MAX, NULL},
  {"hazard_max", "int", NULL, "min=15", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 322, 15,
    INT64_MAX, NULL},
  {"heuristic_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_heuristic_controls_subconfigs, 3,
    confchk_wiredtiger_open_heuristic_controls_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    256, INT64_MIN, INT64_MAX, NULL},
  {"history_store", "category", NULL, NULL, confchk_wiredtiger_open_history_store_subconfigs, 1,
    confchk_wiredtiger_open_history_store_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 260,
    INT64_MIN, INT64_MAX, NULL},
  {"in_memory", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 29,
    INT64_MIN, INT64_MAX, NULL},
  {"io_capacity", "category", NULL, NULL, confchk_wiredtiger_open_io_capacity_subconfigs, 2,
    confchk_wiredtiger_open_io_capacity_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 262,
    INT64_MIN, INT64_MAX, NULL},
  {"json_output", "list", NULL, "choices=[\"error\",\"message\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 265, INT64_MIN, INT64_MAX, confchk_json_output2_choices},
  {"live_restore", "category", NULL, NULL, confchk_wiredtiger_open_live_restore_subconfigs, 4,
    confchk_wiredtiger_open_live_restore_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 64,
    INT64_MIN, INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_wiredtiger_open_log_subconfigs, 12,
    confchk_wiredtiger_open_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"mmap", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 327, INT64_MIN,
    INT64_MAX, NULL},
  {"mmap_all", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 328,
    INT64_MIN, INT64_MAX, NULL},
  {"multiprocess", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 329,
    INT64_MIN, INT64_MAX, NULL},
  {"operation_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 166, 0,
    INT64_MAX, NULL},
  {"operation_tracking", "category", NULL, NULL,
    confchk_wiredtiger_open_operation_tracking_subconfigs, 2,
    confchk_wiredtiger_open_operation_tracking_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    272, INT64_MIN, INT64_MAX, NULL},
  {"page_delta", "category", NULL, NULL, confchk_wiredtiger_open_page_delta_subconfigs, 5,
    confchk_wiredtiger_open_page_delta_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 274,
    INT64_MIN, INT64_MAX, NULL},
  {"precise_checkpoint", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 330,
    INT64_MIN, INT64_MAX, NULL},
  {"prefetch", "category", NULL, NULL, confchk_wiredtiger_open_prefetch_subconfigs, 2,
    confchk_wiredtiger_open_prefetch_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 300,
    INT64_MIN, INT64_MAX, NULL},
  {"preserve_prepared", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 333,
    INT64_MIN, INT64_MAX, NULL},
  {"readonly", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 67, INT64_MIN,
    INT64_MAX, NULL},
  {"rollback_to_stable", "category", NULL, NULL,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs, 1,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    280, INT64_MIN, INT64_MAX, NULL},
  {"salvage", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 334, INT64_MIN,
    INT64_MAX, NULL},
  {"session_max", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 335, 1,
    INT64_MAX, NULL},
  {"session_scratch_max", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 336,
    INT64_MIN, INT64_MAX, NULL},
  {"session_table_cache", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    337, INT64_MIN, INT64_MAX, NULL},
  {"shared_cache", "category", NULL, NULL, confchk_wiredtiger_open_shared_cache_subconfigs, 5,
    confchk_wiredtiger_open_shared_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 282,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics", "list", NULL,
    "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
    "\"clear\",\"tree_walk\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 147, INT64_MIN, INT64_MAX,
    confchk_statistics3_choices},
  {"statistics_log", "category", NULL, NULL, confchk_wiredtiger_open_statistics_log_subconfigs, 6,
    confchk_wiredtiger_open_statistics_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 286,
    INT64_MIN, INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_tiered_storage_subconfigs, 8,
    confchk_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51, INT64_MIN,
    INT64_MAX, NULL},
  {"timing_stress_for_test", "list", NULL,
    "choices=[\"aggressive_stash_free\",\"aggressive_sweep\","
    "\"backup_rename\",\"checkpoint_evict_page\","
    "\"checkpoint_handle\",\"checkpoint_slow\",\"checkpoint_stop\","
    "\"commit_transaction_slow\",\"compact_slow\","
    "\"conn_close_stress_log_printf\",\"evict_reposition\","
    "\"failpoint_eviction_split\","
    "\"failpoint_history_store_delete_key_from_ts\","
    "\"history_store_checkpoint_delay\",\"history_store_search\","
    "\"history_store_sweep_race\",\"live_restore_clean_up\","
    "\"open_index_slow\",\"prefetch_1\",\"prefetch_2\",\"prefetch_3\""
    ",\"prefix_compare\",\"prepare_checkpoint_delay\","
    "\"prepare_resolution_1\",\"prepare_resolution_2\","
    "\"session_alter_slow\",\"sleep_before_read_overflow_onpage\","
    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
    "\"split_6\",\"split_7\",\"split_8\",\"tiered_flush_finish\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 291, INT64_MIN, INT64_MAX,
    confchk_timing_stress_for_test2_choices},
  {"transaction_sync", "category", NULL, NULL, confchk_wiredtiger_open_transaction_sync_subconfigs,
    2, confchk_wiredtiger_open_transaction_sync_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    339, INT64_MIN, INT64_MAX, NULL},
  {"use_environment", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 340,
    INT64_MIN, INT64_MAX, NULL},
  {"use_environment_priv", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    341, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL,
    "choices=[\"all\",\"api\",\"backup\",\"block\","
    "\"block_cache\",\"checkpoint\",\"checkpoint_cleanup\","
    "\"checkpoint_progress\",\"chunkcache\",\"compact\","
    "\"compact_progress\",\"configuration\",\"disaggregated_storage\""
    ",\"error_returns\",\"eviction\",\"fileops\",\"generation\","
    "\"handleops\",\"history_store\",\"history_store_activity\","
    "\"layered\",\"live_restore\",\"live_restore_progress\",\"log\","
    "\"metadata\",\"mutex\",\"out_of_order\",\"overflow\","
    "\"page_delta\",\"prefetch\",\"read\",\"reconcile\",\"recovery\","
    "\"recovery_progress\",\"rts\",\"salvage\",\"shared_cache\","
    "\"split\",\"sweep\",\"temporary\",\"thread_group\",\"tiered\","
    "\"timestamp\",\"transaction\",\"verify\",\"version\",\"write\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX,
    confchk_verbose13_choices},
  {"verify_metadata", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 342,
    INT64_MIN, INT64_MAX, NULL},
  {"write_through", "list", NULL, "choices=[\"data\",\"log\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 343, INT64_MIN, INT64_MAX, confchk_write_through_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 18, 21, 34, 36,
  37, 41, 43, 44, 44, 46, 49, 49, 51, 55, 55, 57, 64, 67, 69, 71, 72, 72, 72, 72, 72, 72, 72, 72};

static const char *confchk_extra_diagnostics3_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_checkpoint_validate, __WT_CONFIG_CHOICE_cursor_check,
  __WT_CONFIG_CHOICE_disk_validate, __WT_CONFIG_CHOICE_eviction_check,
  __WT_CONFIG_CHOICE_generation_check, __WT_CONFIG_CHOICE_hs_validate,
  __WT_CONFIG_CHOICE_key_out_of_order, __WT_CONFIG_CHOICE_log_validate, __WT_CONFIG_CHOICE_prepared,
  __WT_CONFIG_CHOICE_slow_operation, __WT_CONFIG_CHOICE_txn_visibility, NULL};

static const char *confchk_file_extend2_choices[] = {
  __WT_CONFIG_CHOICE_data, __WT_CONFIG_CHOICE_log, NULL};

static const char *confchk_json_output3_choices[] = {
  __WT_CONFIG_CHOICE_error, __WT_CONFIG_CHOICE_message, NULL};

static const char *confchk_statistics4_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_cache_walk, __WT_CONFIG_CHOICE_fast, __WT_CONFIG_CHOICE_none,
  __WT_CONFIG_CHOICE_clear, __WT_CONFIG_CHOICE_tree_walk, NULL};

static const char *confchk_timing_stress_for_test3_choices[] = {
  __WT_CONFIG_CHOICE_aggressive_stash_free, __WT_CONFIG_CHOICE_aggressive_sweep,
  __WT_CONFIG_CHOICE_backup_rename, __WT_CONFIG_CHOICE_checkpoint_evict_page,
  __WT_CONFIG_CHOICE_checkpoint_handle, __WT_CONFIG_CHOICE_checkpoint_slow,
  __WT_CONFIG_CHOICE_checkpoint_stop, __WT_CONFIG_CHOICE_commit_transaction_slow,
  __WT_CONFIG_CHOICE_compact_slow, __WT_CONFIG_CHOICE_conn_close_stress_log_printf,
  __WT_CONFIG_CHOICE_evict_reposition, __WT_CONFIG_CHOICE_failpoint_eviction_split,
  __WT_CONFIG_CHOICE_failpoint_history_store_delete_key_from_ts,
  __WT_CONFIG_CHOICE_history_store_checkpoint_delay, __WT_CONFIG_CHOICE_history_store_search,
  __WT_CONFIG_CHOICE_history_store_sweep_race, __WT_CONFIG_CHOICE_live_restore_clean_up,
  __WT_CONFIG_CHOICE_open_index_slow, __WT_CONFIG_CHOICE_prefetch_1, __WT_CONFIG_CHOICE_prefetch_2,
  __WT_CONFIG_CHOICE_prefetch_3, __WT_CONFIG_CHOICE_prefix_compare,
  __WT_CONFIG_CHOICE_prepare_checkpoint_delay, __WT_CONFIG_CHOICE_prepare_resolution_1,
  __WT_CONFIG_CHOICE_prepare_resolution_2, __WT_CONFIG_CHOICE_session_alter_slow,
  __WT_CONFIG_CHOICE_sleep_before_read_overflow_onpage, __WT_CONFIG_CHOICE_split_1,
  __WT_CONFIG_CHOICE_split_2, __WT_CONFIG_CHOICE_split_3, __WT_CONFIG_CHOICE_split_4,
  __WT_CONFIG_CHOICE_split_5, __WT_CONFIG_CHOICE_split_6, __WT_CONFIG_CHOICE_split_7,
  __WT_CONFIG_CHOICE_split_8, __WT_CONFIG_CHOICE_tiered_flush_finish, NULL};

static const char *confchk_verbose14_choices[] = {__WT_CONFIG_CHOICE_all, __WT_CONFIG_CHOICE_api,
  __WT_CONFIG_CHOICE_backup, __WT_CONFIG_CHOICE_block, __WT_CONFIG_CHOICE_block_cache,
  __WT_CONFIG_CHOICE_checkpoint, __WT_CONFIG_CHOICE_checkpoint_cleanup,
  __WT_CONFIG_CHOICE_checkpoint_progress, __WT_CONFIG_CHOICE_chunkcache, __WT_CONFIG_CHOICE_compact,
  __WT_CONFIG_CHOICE_compact_progress, __WT_CONFIG_CHOICE_configuration,
  __WT_CONFIG_CHOICE_disaggregated_storage, __WT_CONFIG_CHOICE_error_returns,
  __WT_CONFIG_CHOICE_eviction, __WT_CONFIG_CHOICE_fileops, __WT_CONFIG_CHOICE_generation,
  __WT_CONFIG_CHOICE_handleops, __WT_CONFIG_CHOICE_history_store,
  __WT_CONFIG_CHOICE_history_store_activity, __WT_CONFIG_CHOICE_layered,
  __WT_CONFIG_CHOICE_live_restore, __WT_CONFIG_CHOICE_live_restore_progress, __WT_CONFIG_CHOICE_log,
  __WT_CONFIG_CHOICE_metadata, __WT_CONFIG_CHOICE_mutex, __WT_CONFIG_CHOICE_out_of_order,
  __WT_CONFIG_CHOICE_overflow, __WT_CONFIG_CHOICE_page_delta, __WT_CONFIG_CHOICE_prefetch,
  __WT_CONFIG_CHOICE_read, __WT_CONFIG_CHOICE_reconcile, __WT_CONFIG_CHOICE_recovery,
  __WT_CONFIG_CHOICE_recovery_progress, __WT_CONFIG_CHOICE_rts, __WT_CONFIG_CHOICE_salvage,
  __WT_CONFIG_CHOICE_shared_cache, __WT_CONFIG_CHOICE_split, __WT_CONFIG_CHOICE_sweep,
  __WT_CONFIG_CHOICE_temporary, __WT_CONFIG_CHOICE_thread_group, __WT_CONFIG_CHOICE_tiered,
  __WT_CONFIG_CHOICE_timestamp, __WT_CONFIG_CHOICE_transaction, __WT_CONFIG_CHOICE_verify,
  __WT_CONFIG_CHOICE_version, __WT_CONFIG_CHOICE_write, NULL};

static const char *confchk_write_through2_choices[] = {
  __WT_CONFIG_CHOICE_data, __WT_CONFIG_CHOICE_log, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_all[] = {
  {"backup_restore_target", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 302,
    INT64_MIN, INT64_MAX, NULL},
  {"block_cache", "category", NULL, NULL, confchk_wiredtiger_open_block_cache_subconfigs, 12,
    confchk_wiredtiger_open_block_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 189,
    INT64_MIN, INT64_MAX, NULL},
  {"buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    303, -1, 1LL * WT_MEGABYTE, NULL},
  {"builtin_extension_config", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    304, INT64_MIN, INT64_MAX, NULL},
  {"cache_cursors", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 296,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_eviction_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs, 2,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 200, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 203, 0,
    INT64_MAX, NULL},
  {"cache_overhead", "int", NULL, "min=0,max=30", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 204,
    0, 30, NULL},
  {"cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 205,
    1LL * WT_MEGABYTE, 10LL * WT_TERABYTE, NULL},
  {"cache_stuck_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 206,
    0, INT64_MAX, NULL},
  {"checkpoint", "category", NULL, NULL, confchk_wiredtiger_open_checkpoint_subconfigs, 2,
    confchk_wiredtiger_open_checkpoint_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 207,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_cleanup", "category", NULL, NULL,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs, 3,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    210, INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_sync", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 305,
    INT64_MIN, INT64_MAX, NULL},
  {"chunk_cache", "category", NULL, NULL, confchk_wiredtiger_open_chunk_cache_subconfigs, 9,
    confchk_wiredtiger_open_chunk_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 213,
    INT64_MIN, INT64_MAX, NULL},
  {"compatibility", "category", NULL, NULL, confchk_wiredtiger_open_compatibility_subconfigs, 3,
    confchk_wiredtiger_open_compatibility_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 215,
    INT64_MIN, INT64_MAX, NULL},
  {"compile_configuration_count", "int", NULL, "min=500", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 312, 500, INT64_MAX, NULL},
  {"config_base", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 313,
    INT64_MIN, INT64_MAX, NULL},
  {"create", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 314, INT64_MIN,
    INT64_MAX, NULL},
  {"debug_mode", "category", NULL, NULL, confchk_wiredtiger_open_debug_mode_subconfigs, 18,
    confchk_wiredtiger_open_debug_mode_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 217,
    INT64_MIN, INT64_MAX, NULL},
  {"direct_io", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 315, INT64_MIN,
    INT64_MAX, NULL},
  {"disaggregated", "category", NULL, NULL, confchk_wiredtiger_open_disaggregated_subconfigs, 6,
    confchk_wiredtiger_open_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_wiredtiger_open_encryption_subconfigs, 3,
    confchk_wiredtiger_open_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"error_prefix", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 236,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction", "category", NULL, NULL, confchk_wiredtiger_open_eviction_subconfigs, 5,
    confchk_wiredtiger_open_eviction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 237,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction_checkpoint_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 243, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_target", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 244, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_trigger", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 245, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    246, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_trigger", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    247, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 248, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_trigger", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 249, 0, 10LL * WT_TERABYTE, NULL},
  {"exclusive", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 98,
    INT64_MIN, INT64_MAX, NULL},
  {"extensions", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 317, INT64_MIN,
    INT64_MAX, NULL},
  {"extra_diagnostics", "list", NULL,
    "choices=[\"all\",\"checkpoint_validate\",\"cursor_check\""
    ",\"disk_validate\",\"eviction_check\",\"generation_check\","
    "\"hs_validate\",\"key_out_of_order\",\"log_validate\","
    "\"prepared\",\"slow_operation\",\"txn_visibility\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 250, INT64_MIN, INT64_MAX,
    confchk_extra_diagnostics3_choices},
  {"file_extend", "list", NULL, "choices=[\"data\",\"log\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 318, INT64_MIN, INT64_MAX, confchk_file_extend2_choices},
  {"file_manager", "category", NULL, NULL, confchk_wiredtiger_open_file_manager_subconfigs, 3,
    confchk_wiredtiger_open_file_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 251,
    INT64_MIN, INT64_MAX, NULL},
  {"generation_drain_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    255, 0, INT64_MAX, NULL},
  {"hash", "category", NULL, NULL, confchk_wiredtiger_open_hash_subconfigs, 2,
    confchk_wiredtiger_open_hash_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 319, INT64_MIN,
    INT64_MAX, NULL},
  {"hazard_max", "int", NULL, "min=15", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 322, 15,
    INT64_MAX, NULL},
  {"heuristic_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_heuristic_controls_subconfigs, 3,
    confchk_wiredtiger_open_heuristic_controls_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    256, INT64_MIN, INT64_MAX, NULL},
  {"history_store", "category", NULL, NULL, confchk_wiredtiger_open_history_store_subconfigs, 1,
    confchk_wiredtiger_open_history_store_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 260,
    INT64_MIN, INT64_MAX, NULL},
  {"in_memory", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 29,
    INT64_MIN, INT64_MAX, NULL},
  {"io_capacity", "category", NULL, NULL, confchk_wiredtiger_open_io_capacity_subconfigs, 2,
    confchk_wiredtiger_open_io_capacity_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 262,
    INT64_MIN, INT64_MAX, NULL},
  {"json_output", "list", NULL, "choices=[\"error\",\"message\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 265, INT64_MIN, INT64_MAX, confchk_json_output3_choices},
  {"live_restore", "category", NULL, NULL, confchk_wiredtiger_open_live_restore_subconfigs, 4,
    confchk_wiredtiger_open_live_restore_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 64,
    INT64_MIN, INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_wiredtiger_open_log_subconfigs, 12,
    confchk_wiredtiger_open_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"mmap", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 327, INT64_MIN,
    INT64_MAX, NULL},
  {"mmap_all", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 328,
    INT64_MIN, INT64_MAX, NULL},
  {"multiprocess", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 329,
    INT64_MIN, INT64_MAX, NULL},
  {"operation_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 166, 0,
    INT64_MAX, NULL},
  {"operation_tracking", "category", NULL, NULL,
    confchk_wiredtiger_open_operation_tracking_subconfigs, 2,
    confchk_wiredtiger_open_operation_tracking_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    272, INT64_MIN, INT64_MAX, NULL},
  {"page_delta", "category", NULL, NULL, confchk_wiredtiger_open_page_delta_subconfigs, 5,
    confchk_wiredtiger_open_page_delta_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 274,
    INT64_MIN, INT64_MAX, NULL},
  {"precise_checkpoint", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 330,
    INT64_MIN, INT64_MAX, NULL},
  {"prefetch", "category", NULL, NULL, confchk_wiredtiger_open_prefetch_subconfigs, 2,
    confchk_wiredtiger_open_prefetch_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 300,
    INT64_MIN, INT64_MAX, NULL},
  {"preserve_prepared", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 333,
    INT64_MIN, INT64_MAX, NULL},
  {"readonly", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 67, INT64_MIN,
    INT64_MAX, NULL},
  {"rollback_to_stable", "category", NULL, NULL,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs, 1,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    280, INT64_MIN, INT64_MAX, NULL},
  {"salvage", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 334, INT64_MIN,
    INT64_MAX, NULL},
  {"session_max", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 335, 1,
    INT64_MAX, NULL},
  {"session_scratch_max", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 336,
    INT64_MIN, INT64_MAX, NULL},
  {"session_table_cache", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    337, INT64_MIN, INT64_MAX, NULL},
  {"shared_cache", "category", NULL, NULL, confchk_wiredtiger_open_shared_cache_subconfigs, 5,
    confchk_wiredtiger_open_shared_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 282,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics", "list", NULL,
    "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
    "\"clear\",\"tree_walk\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 147, INT64_MIN, INT64_MAX,
    confchk_statistics4_choices},
  {"statistics_log", "category", NULL, NULL, confchk_wiredtiger_open_statistics_log_subconfigs, 6,
    confchk_wiredtiger_open_statistics_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 286,
    INT64_MIN, INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_tiered_storage_subconfigs, 8,
    confchk_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51, INT64_MIN,
    INT64_MAX, NULL},
  {"timing_stress_for_test", "list", NULL,
    "choices=[\"aggressive_stash_free\",\"aggressive_sweep\","
    "\"backup_rename\",\"checkpoint_evict_page\","
    "\"checkpoint_handle\",\"checkpoint_slow\",\"checkpoint_stop\","
    "\"commit_transaction_slow\",\"compact_slow\","
    "\"conn_close_stress_log_printf\",\"evict_reposition\","
    "\"failpoint_eviction_split\","
    "\"failpoint_history_store_delete_key_from_ts\","
    "\"history_store_checkpoint_delay\",\"history_store_search\","
    "\"history_store_sweep_race\",\"live_restore_clean_up\","
    "\"open_index_slow\",\"prefetch_1\",\"prefetch_2\",\"prefetch_3\""
    ",\"prefix_compare\",\"prepare_checkpoint_delay\","
    "\"prepare_resolution_1\",\"prepare_resolution_2\","
    "\"session_alter_slow\",\"sleep_before_read_overflow_onpage\","
    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
    "\"split_6\",\"split_7\",\"split_8\",\"tiered_flush_finish\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 291, INT64_MIN, INT64_MAX,
    confchk_timing_stress_for_test3_choices},
  {"transaction_sync", "category", NULL, NULL, confchk_wiredtiger_open_transaction_sync_subconfigs,
    2, confchk_wiredtiger_open_transaction_sync_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    339, INT64_MIN, INT64_MAX, NULL},
  {"use_environment", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 340,
    INT64_MIN, INT64_MAX, NULL},
  {"use_environment_priv", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    341, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL,
    "choices=[\"all\",\"api\",\"backup\",\"block\","
    "\"block_cache\",\"checkpoint\",\"checkpoint_cleanup\","
    "\"checkpoint_progress\",\"chunkcache\",\"compact\","
    "\"compact_progress\",\"configuration\",\"disaggregated_storage\""
    ",\"error_returns\",\"eviction\",\"fileops\",\"generation\","
    "\"handleops\",\"history_store\",\"history_store_activity\","
    "\"layered\",\"live_restore\",\"live_restore_progress\",\"log\","
    "\"metadata\",\"mutex\",\"out_of_order\",\"overflow\","
    "\"page_delta\",\"prefetch\",\"read\",\"reconcile\",\"recovery\","
    "\"recovery_progress\",\"rts\",\"salvage\",\"shared_cache\","
    "\"split\",\"sweep\",\"temporary\",\"thread_group\",\"tiered\","
    "\"timestamp\",\"transaction\",\"verify\",\"version\",\"write\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX,
    confchk_verbose14_choices},
  {"verify_metadata", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 342,
    INT64_MIN, INT64_MAX, NULL},
  {"version", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 69, INT64_MIN,
    INT64_MAX, NULL},
  {"write_through", "list", NULL, "choices=[\"data\",\"log\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 343, INT64_MIN, INT64_MAX, confchk_write_through2_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_all_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 18, 21, 34,
  36, 37, 41, 43, 44, 44, 46, 49, 49, 51, 55, 55, 57, 64, 67, 69, 72, 73, 73, 73, 73, 73, 73, 73,
  73};

static const char *confchk_extra_diagnostics4_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_checkpoint_validate, __WT_CONFIG_CHOICE_cursor_check,
  __WT_CONFIG_CHOICE_disk_validate, __WT_CONFIG_CHOICE_eviction_check,
  __WT_CONFIG_CHOICE_generation_check, __WT_CONFIG_CHOICE_hs_validate,
  __WT_CONFIG_CHOICE_key_out_of_order, __WT_CONFIG_CHOICE_log_validate, __WT_CONFIG_CHOICE_prepared,
  __WT_CONFIG_CHOICE_slow_operation, __WT_CONFIG_CHOICE_txn_visibility, NULL};

static const char *confchk_file_extend3_choices[] = {
  __WT_CONFIG_CHOICE_data, __WT_CONFIG_CHOICE_log, NULL};

static const char *confchk_json_output4_choices[] = {
  __WT_CONFIG_CHOICE_error, __WT_CONFIG_CHOICE_message, NULL};

static const char *confchk_statistics5_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_cache_walk, __WT_CONFIG_CHOICE_fast, __WT_CONFIG_CHOICE_none,
  __WT_CONFIG_CHOICE_clear, __WT_CONFIG_CHOICE_tree_walk, NULL};

static const char *confchk_timing_stress_for_test4_choices[] = {
  __WT_CONFIG_CHOICE_aggressive_stash_free, __WT_CONFIG_CHOICE_aggressive_sweep,
  __WT_CONFIG_CHOICE_backup_rename, __WT_CONFIG_CHOICE_checkpoint_evict_page,
  __WT_CONFIG_CHOICE_checkpoint_handle, __WT_CONFIG_CHOICE_checkpoint_slow,
  __WT_CONFIG_CHOICE_checkpoint_stop, __WT_CONFIG_CHOICE_commit_transaction_slow,
  __WT_CONFIG_CHOICE_compact_slow, __WT_CONFIG_CHOICE_conn_close_stress_log_printf,
  __WT_CONFIG_CHOICE_evict_reposition, __WT_CONFIG_CHOICE_failpoint_eviction_split,
  __WT_CONFIG_CHOICE_failpoint_history_store_delete_key_from_ts,
  __WT_CONFIG_CHOICE_history_store_checkpoint_delay, __WT_CONFIG_CHOICE_history_store_search,
  __WT_CONFIG_CHOICE_history_store_sweep_race, __WT_CONFIG_CHOICE_live_restore_clean_up,
  __WT_CONFIG_CHOICE_open_index_slow, __WT_CONFIG_CHOICE_prefetch_1, __WT_CONFIG_CHOICE_prefetch_2,
  __WT_CONFIG_CHOICE_prefetch_3, __WT_CONFIG_CHOICE_prefix_compare,
  __WT_CONFIG_CHOICE_prepare_checkpoint_delay, __WT_CONFIG_CHOICE_prepare_resolution_1,
  __WT_CONFIG_CHOICE_prepare_resolution_2, __WT_CONFIG_CHOICE_session_alter_slow,
  __WT_CONFIG_CHOICE_sleep_before_read_overflow_onpage, __WT_CONFIG_CHOICE_split_1,
  __WT_CONFIG_CHOICE_split_2, __WT_CONFIG_CHOICE_split_3, __WT_CONFIG_CHOICE_split_4,
  __WT_CONFIG_CHOICE_split_5, __WT_CONFIG_CHOICE_split_6, __WT_CONFIG_CHOICE_split_7,
  __WT_CONFIG_CHOICE_split_8, __WT_CONFIG_CHOICE_tiered_flush_finish, NULL};

static const char *confchk_verbose15_choices[] = {__WT_CONFIG_CHOICE_all, __WT_CONFIG_CHOICE_api,
  __WT_CONFIG_CHOICE_backup, __WT_CONFIG_CHOICE_block, __WT_CONFIG_CHOICE_block_cache,
  __WT_CONFIG_CHOICE_checkpoint, __WT_CONFIG_CHOICE_checkpoint_cleanup,
  __WT_CONFIG_CHOICE_checkpoint_progress, __WT_CONFIG_CHOICE_chunkcache, __WT_CONFIG_CHOICE_compact,
  __WT_CONFIG_CHOICE_compact_progress, __WT_CONFIG_CHOICE_configuration,
  __WT_CONFIG_CHOICE_disaggregated_storage, __WT_CONFIG_CHOICE_error_returns,
  __WT_CONFIG_CHOICE_eviction, __WT_CONFIG_CHOICE_fileops, __WT_CONFIG_CHOICE_generation,
  __WT_CONFIG_CHOICE_handleops, __WT_CONFIG_CHOICE_history_store,
  __WT_CONFIG_CHOICE_history_store_activity, __WT_CONFIG_CHOICE_layered,
  __WT_CONFIG_CHOICE_live_restore, __WT_CONFIG_CHOICE_live_restore_progress, __WT_CONFIG_CHOICE_log,
  __WT_CONFIG_CHOICE_metadata, __WT_CONFIG_CHOICE_mutex, __WT_CONFIG_CHOICE_out_of_order,
  __WT_CONFIG_CHOICE_overflow, __WT_CONFIG_CHOICE_page_delta, __WT_CONFIG_CHOICE_prefetch,
  __WT_CONFIG_CHOICE_read, __WT_CONFIG_CHOICE_reconcile, __WT_CONFIG_CHOICE_recovery,
  __WT_CONFIG_CHOICE_recovery_progress, __WT_CONFIG_CHOICE_rts, __WT_CONFIG_CHOICE_salvage,
  __WT_CONFIG_CHOICE_shared_cache, __WT_CONFIG_CHOICE_split, __WT_CONFIG_CHOICE_sweep,
  __WT_CONFIG_CHOICE_temporary, __WT_CONFIG_CHOICE_thread_group, __WT_CONFIG_CHOICE_tiered,
  __WT_CONFIG_CHOICE_timestamp, __WT_CONFIG_CHOICE_transaction, __WT_CONFIG_CHOICE_verify,
  __WT_CONFIG_CHOICE_version, __WT_CONFIG_CHOICE_write, NULL};

static const char *confchk_write_through3_choices[] = {
  __WT_CONFIG_CHOICE_data, __WT_CONFIG_CHOICE_log, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_basecfg[] = {
  {"backup_restore_target", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 302,
    INT64_MIN, INT64_MAX, NULL},
  {"block_cache", "category", NULL, NULL, confchk_wiredtiger_open_block_cache_subconfigs, 12,
    confchk_wiredtiger_open_block_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 189,
    INT64_MIN, INT64_MAX, NULL},
  {"buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    303, -1, 1LL * WT_MEGABYTE, NULL},
  {"builtin_extension_config", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    304, INT64_MIN, INT64_MAX, NULL},
  {"cache_cursors", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 296,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_eviction_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs, 2,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 200, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 203, 0,
    INT64_MAX, NULL},
  {"cache_overhead", "int", NULL, "min=0,max=30", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 204,
    0, 30, NULL},
  {"cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 205,
    1LL * WT_MEGABYTE, 10LL * WT_TERABYTE, NULL},
  {"cache_stuck_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 206,
    0, INT64_MAX, NULL},
  {"checkpoint", "category", NULL, NULL, confchk_wiredtiger_open_checkpoint_subconfigs, 2,
    confchk_wiredtiger_open_checkpoint_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 207,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_cleanup", "category", NULL, NULL,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs, 3,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    210, INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_sync", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 305,
    INT64_MIN, INT64_MAX, NULL},
  {"chunk_cache", "category", NULL, NULL, confchk_wiredtiger_open_chunk_cache_subconfigs, 9,
    confchk_wiredtiger_open_chunk_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 213,
    INT64_MIN, INT64_MAX, NULL},
  {"compatibility", "category", NULL, NULL, confchk_wiredtiger_open_compatibility_subconfigs, 3,
    confchk_wiredtiger_open_compatibility_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 215,
    INT64_MIN, INT64_MAX, NULL},
  {"compile_configuration_count", "int", NULL, "min=500", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 312, 500, INT64_MAX, NULL},
  {"debug_mode", "category", NULL, NULL, confchk_wiredtiger_open_debug_mode_subconfigs, 18,
    confchk_wiredtiger_open_debug_mode_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 217,
    INT64_MIN, INT64_MAX, NULL},
  {"direct_io", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 315, INT64_MIN,
    INT64_MAX, NULL},
  {"disaggregated", "category", NULL, NULL, confchk_wiredtiger_open_disaggregated_subconfigs, 6,
    confchk_wiredtiger_open_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_wiredtiger_open_encryption_subconfigs, 3,
    confchk_wiredtiger_open_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"error_prefix", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 236,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction", "category", NULL, NULL, confchk_wiredtiger_open_eviction_subconfigs, 5,
    confchk_wiredtiger_open_eviction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 237,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction_checkpoint_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 243, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_target", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 244, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_trigger", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 245, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    246, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_trigger", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    247, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 248, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_trigger", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 249, 0, 10LL * WT_TERABYTE, NULL},
  {"extensions", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 317, INT64_MIN,
    INT64_MAX, NULL},
  {"extra_diagnostics", "list", NULL,
    "choices=[\"all\",\"checkpoint_validate\",\"cursor_check\""
    ",\"disk_validate\",\"eviction_check\",\"generation_check\","
    "\"hs_validate\",\"key_out_of_order\",\"log_validate\","
    "\"prepared\",\"slow_operation\",\"txn_visibility\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 250, INT64_MIN, INT64_MAX,
    confchk_extra_diagnostics4_choices},
  {"file_extend", "list", NULL, "choices=[\"data\",\"log\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 318, INT64_MIN, INT64_MAX, confchk_file_extend3_choices},
  {"file_manager", "category", NULL, NULL, confchk_wiredtiger_open_file_manager_subconfigs, 3,
    confchk_wiredtiger_open_file_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 251,
    INT64_MIN, INT64_MAX, NULL},
  {"generation_drain_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    255, 0, INT64_MAX, NULL},
  {"hash", "category", NULL, NULL, confchk_wiredtiger_open_hash_subconfigs, 2,
    confchk_wiredtiger_open_hash_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 319, INT64_MIN,
    INT64_MAX, NULL},
  {"hazard_max", "int", NULL, "min=15", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 322, 15,
    INT64_MAX, NULL},
  {"heuristic_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_heuristic_controls_subconfigs, 3,
    confchk_wiredtiger_open_heuristic_controls_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    256, INT64_MIN, INT64_MAX, NULL},
  {"history_store", "category", NULL, NULL, confchk_wiredtiger_open_history_store_subconfigs, 1,
    confchk_wiredtiger_open_history_store_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 260,
    INT64_MIN, INT64_MAX, NULL},
  {"io_capacity", "category", NULL, NULL, confchk_wiredtiger_open_io_capacity_subconfigs, 2,
    confchk_wiredtiger_open_io_capacity_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 262,
    INT64_MIN, INT64_MAX, NULL},
  {"json_output", "list", NULL, "choices=[\"error\",\"message\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 265, INT64_MIN, INT64_MAX, confchk_json_output4_choices},
  {"live_restore", "category", NULL, NULL, confchk_wiredtiger_open_live_restore_subconfigs, 4,
    confchk_wiredtiger_open_live_restore_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 64,
    INT64_MIN, INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_wiredtiger_open_log_subconfigs, 12,
    confchk_wiredtiger_open_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"mmap", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 327, INT64_MIN,
    INT64_MAX, NULL},
  {"mmap_all", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 328,
    INT64_MIN, INT64_MAX, NULL},
  {"multiprocess", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 329,
    INT64_MIN, INT64_MAX, NULL},
  {"operation_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 166, 0,
    INT64_MAX, NULL},
  {"operation_tracking", "category", NULL, NULL,
    confchk_wiredtiger_open_operation_tracking_subconfigs, 2,
    confchk_wiredtiger_open_operation_tracking_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    272, INT64_MIN, INT64_MAX, NULL},
  {"page_delta", "category", NULL, NULL, confchk_wiredtiger_open_page_delta_subconfigs, 5,
    confchk_wiredtiger_open_page_delta_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 274,
    INT64_MIN, INT64_MAX, NULL},
  {"precise_checkpoint", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 330,
    INT64_MIN, INT64_MAX, NULL},
  {"prefetch", "category", NULL, NULL, confchk_wiredtiger_open_prefetch_subconfigs, 2,
    confchk_wiredtiger_open_prefetch_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 300,
    INT64_MIN, INT64_MAX, NULL},
  {"preserve_prepared", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 333,
    INT64_MIN, INT64_MAX, NULL},
  {"readonly", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 67, INT64_MIN,
    INT64_MAX, NULL},
  {"rollback_to_stable", "category", NULL, NULL,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs, 1,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    280, INT64_MIN, INT64_MAX, NULL},
  {"salvage", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 334, INT64_MIN,
    INT64_MAX, NULL},
  {"session_max", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 335, 1,
    INT64_MAX, NULL},
  {"session_scratch_max", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 336,
    INT64_MIN, INT64_MAX, NULL},
  {"session_table_cache", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    337, INT64_MIN, INT64_MAX, NULL},
  {"shared_cache", "category", NULL, NULL, confchk_wiredtiger_open_shared_cache_subconfigs, 5,
    confchk_wiredtiger_open_shared_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 282,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics", "list", NULL,
    "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
    "\"clear\",\"tree_walk\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 147, INT64_MIN, INT64_MAX,
    confchk_statistics5_choices},
  {"statistics_log", "category", NULL, NULL, confchk_wiredtiger_open_statistics_log_subconfigs, 6,
    confchk_wiredtiger_open_statistics_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 286,
    INT64_MIN, INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_tiered_storage_subconfigs, 8,
    confchk_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51, INT64_MIN,
    INT64_MAX, NULL},
  {"timing_stress_for_test", "list", NULL,
    "choices=[\"aggressive_stash_free\",\"aggressive_sweep\","
    "\"backup_rename\",\"checkpoint_evict_page\","
    "\"checkpoint_handle\",\"checkpoint_slow\",\"checkpoint_stop\","
    "\"commit_transaction_slow\",\"compact_slow\","
    "\"conn_close_stress_log_printf\",\"evict_reposition\","
    "\"failpoint_eviction_split\","
    "\"failpoint_history_store_delete_key_from_ts\","
    "\"history_store_checkpoint_delay\",\"history_store_search\","
    "\"history_store_sweep_race\",\"live_restore_clean_up\","
    "\"open_index_slow\",\"prefetch_1\",\"prefetch_2\",\"prefetch_3\""
    ",\"prefix_compare\",\"prepare_checkpoint_delay\","
    "\"prepare_resolution_1\",\"prepare_resolution_2\","
    "\"session_alter_slow\",\"sleep_before_read_overflow_onpage\","
    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
    "\"split_6\",\"split_7\",\"split_8\",\"tiered_flush_finish\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 291, INT64_MIN, INT64_MAX,
    confchk_timing_stress_for_test4_choices},
  {"transaction_sync", "category", NULL, NULL, confchk_wiredtiger_open_transaction_sync_subconfigs,
    2, confchk_wiredtiger_open_transaction_sync_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    339, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL,
    "choices=[\"all\",\"api\",\"backup\",\"block\","
    "\"block_cache\",\"checkpoint\",\"checkpoint_cleanup\","
    "\"checkpoint_progress\",\"chunkcache\",\"compact\","
    "\"compact_progress\",\"configuration\",\"disaggregated_storage\""
    ",\"error_returns\",\"eviction\",\"fileops\",\"generation\","
    "\"handleops\",\"history_store\",\"history_store_activity\","
    "\"layered\",\"live_restore\",\"live_restore_progress\",\"log\","
    "\"metadata\",\"mutex\",\"out_of_order\",\"overflow\","
    "\"page_delta\",\"prefetch\",\"read\",\"reconcile\",\"recovery\","
    "\"recovery_progress\",\"rts\",\"salvage\",\"shared_cache\","
    "\"split\",\"sweep\",\"temporary\",\"thread_group\",\"tiered\","
    "\"timestamp\",\"transaction\",\"verify\",\"version\",\"write\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX,
    confchk_verbose15_choices},
  {"verify_metadata", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 342,
    INT64_MIN, INT64_MAX, NULL},
  {"version", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 69, INT64_MIN,
    INT64_MAX, NULL},
  {"write_through", "list", NULL, "choices=[\"data\",\"log\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 343, INT64_MIN, INT64_MAX, confchk_write_through3_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_basecfg_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 16, 19,
  31, 33, 34, 38, 39, 40, 40, 42, 45, 45, 47, 51, 51, 53, 60, 63, 63, 66, 67, 67, 67, 67, 67, 67,
  67, 67};

static const char *confchk_extra_diagnostics5_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_checkpoint_validate, __WT_CONFIG_CHOICE_cursor_check,
  __WT_CONFIG_CHOICE_disk_validate, __WT_CONFIG_CHOICE_eviction_check,
  __WT_CONFIG_CHOICE_generation_check, __WT_CONFIG_CHOICE_hs_validate,
  __WT_CONFIG_CHOICE_key_out_of_order, __WT_CONFIG_CHOICE_log_validate, __WT_CONFIG_CHOICE_prepared,
  __WT_CONFIG_CHOICE_slow_operation, __WT_CONFIG_CHOICE_txn_visibility, NULL};

static const char *confchk_file_extend4_choices[] = {
  __WT_CONFIG_CHOICE_data, __WT_CONFIG_CHOICE_log, NULL};

static const char *confchk_json_output5_choices[] = {
  __WT_CONFIG_CHOICE_error, __WT_CONFIG_CHOICE_message, NULL};

static const char *confchk_statistics6_choices[] = {__WT_CONFIG_CHOICE_all,
  __WT_CONFIG_CHOICE_cache_walk, __WT_CONFIG_CHOICE_fast, __WT_CONFIG_CHOICE_none,
  __WT_CONFIG_CHOICE_clear, __WT_CONFIG_CHOICE_tree_walk, NULL};

static const char *confchk_timing_stress_for_test5_choices[] = {
  __WT_CONFIG_CHOICE_aggressive_stash_free, __WT_CONFIG_CHOICE_aggressive_sweep,
  __WT_CONFIG_CHOICE_backup_rename, __WT_CONFIG_CHOICE_checkpoint_evict_page,
  __WT_CONFIG_CHOICE_checkpoint_handle, __WT_CONFIG_CHOICE_checkpoint_slow,
  __WT_CONFIG_CHOICE_checkpoint_stop, __WT_CONFIG_CHOICE_commit_transaction_slow,
  __WT_CONFIG_CHOICE_compact_slow, __WT_CONFIG_CHOICE_conn_close_stress_log_printf,
  __WT_CONFIG_CHOICE_evict_reposition, __WT_CONFIG_CHOICE_failpoint_eviction_split,
  __WT_CONFIG_CHOICE_failpoint_history_store_delete_key_from_ts,
  __WT_CONFIG_CHOICE_history_store_checkpoint_delay, __WT_CONFIG_CHOICE_history_store_search,
  __WT_CONFIG_CHOICE_history_store_sweep_race, __WT_CONFIG_CHOICE_live_restore_clean_up,
  __WT_CONFIG_CHOICE_open_index_slow, __WT_CONFIG_CHOICE_prefetch_1, __WT_CONFIG_CHOICE_prefetch_2,
  __WT_CONFIG_CHOICE_prefetch_3, __WT_CONFIG_CHOICE_prefix_compare,
  __WT_CONFIG_CHOICE_prepare_checkpoint_delay, __WT_CONFIG_CHOICE_prepare_resolution_1,
  __WT_CONFIG_CHOICE_prepare_resolution_2, __WT_CONFIG_CHOICE_session_alter_slow,
  __WT_CONFIG_CHOICE_sleep_before_read_overflow_onpage, __WT_CONFIG_CHOICE_split_1,
  __WT_CONFIG_CHOICE_split_2, __WT_CONFIG_CHOICE_split_3, __WT_CONFIG_CHOICE_split_4,
  __WT_CONFIG_CHOICE_split_5, __WT_CONFIG_CHOICE_split_6, __WT_CONFIG_CHOICE_split_7,
  __WT_CONFIG_CHOICE_split_8, __WT_CONFIG_CHOICE_tiered_flush_finish, NULL};

static const char *confchk_verbose16_choices[] = {__WT_CONFIG_CHOICE_all, __WT_CONFIG_CHOICE_api,
  __WT_CONFIG_CHOICE_backup, __WT_CONFIG_CHOICE_block, __WT_CONFIG_CHOICE_block_cache,
  __WT_CONFIG_CHOICE_checkpoint, __WT_CONFIG_CHOICE_checkpoint_cleanup,
  __WT_CONFIG_CHOICE_checkpoint_progress, __WT_CONFIG_CHOICE_chunkcache, __WT_CONFIG_CHOICE_compact,
  __WT_CONFIG_CHOICE_compact_progress, __WT_CONFIG_CHOICE_configuration,
  __WT_CONFIG_CHOICE_disaggregated_storage, __WT_CONFIG_CHOICE_error_returns,
  __WT_CONFIG_CHOICE_eviction, __WT_CONFIG_CHOICE_fileops, __WT_CONFIG_CHOICE_generation,
  __WT_CONFIG_CHOICE_handleops, __WT_CONFIG_CHOICE_history_store,
  __WT_CONFIG_CHOICE_history_store_activity, __WT_CONFIG_CHOICE_layered,
  __WT_CONFIG_CHOICE_live_restore, __WT_CONFIG_CHOICE_live_restore_progress, __WT_CONFIG_CHOICE_log,
  __WT_CONFIG_CHOICE_metadata, __WT_CONFIG_CHOICE_mutex, __WT_CONFIG_CHOICE_out_of_order,
  __WT_CONFIG_CHOICE_overflow, __WT_CONFIG_CHOICE_page_delta, __WT_CONFIG_CHOICE_prefetch,
  __WT_CONFIG_CHOICE_read, __WT_CONFIG_CHOICE_reconcile, __WT_CONFIG_CHOICE_recovery,
  __WT_CONFIG_CHOICE_recovery_progress, __WT_CONFIG_CHOICE_rts, __WT_CONFIG_CHOICE_salvage,
  __WT_CONFIG_CHOICE_shared_cache, __WT_CONFIG_CHOICE_split, __WT_CONFIG_CHOICE_sweep,
  __WT_CONFIG_CHOICE_temporary, __WT_CONFIG_CHOICE_thread_group, __WT_CONFIG_CHOICE_tiered,
  __WT_CONFIG_CHOICE_timestamp, __WT_CONFIG_CHOICE_transaction, __WT_CONFIG_CHOICE_verify,
  __WT_CONFIG_CHOICE_version, __WT_CONFIG_CHOICE_write, NULL};

static const char *confchk_write_through4_choices[] = {
  __WT_CONFIG_CHOICE_data, __WT_CONFIG_CHOICE_log, NULL};

static const WT_CONFIG_CHECK confchk_wiredtiger_open_usercfg[] = {
  {"backup_restore_target", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 302,
    INT64_MIN, INT64_MAX, NULL},
  {"block_cache", "category", NULL, NULL, confchk_wiredtiger_open_block_cache_subconfigs, 12,
    confchk_wiredtiger_open_block_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 189,
    INT64_MIN, INT64_MAX, NULL},
  {"buffer_alignment", "int", NULL, "min=-1,max=1MB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    303, -1, 1LL * WT_MEGABYTE, NULL},
  {"builtin_extension_config", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    304, INT64_MIN, INT64_MAX, NULL},
  {"cache_cursors", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 296,
    INT64_MIN, INT64_MAX, NULL},
  {"cache_eviction_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs, 2,
    confchk_wiredtiger_open_cache_eviction_controls_subconfigs_jump,
    WT_CONFIG_COMPILED_TYPE_CATEGORY, 200, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 203, 0,
    INT64_MAX, NULL},
  {"cache_overhead", "int", NULL, "min=0,max=30", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 204,
    0, 30, NULL},
  {"cache_size", "int", NULL, "min=1MB,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 205,
    1LL * WT_MEGABYTE, 10LL * WT_TERABYTE, NULL},
  {"cache_stuck_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 206,
    0, INT64_MAX, NULL},
  {"checkpoint", "category", NULL, NULL, confchk_wiredtiger_open_checkpoint_subconfigs, 2,
    confchk_wiredtiger_open_checkpoint_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 207,
    INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_cleanup", "category", NULL, NULL,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs, 3,
    confchk_wiredtiger_open_checkpoint_cleanup_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    210, INT64_MIN, INT64_MAX, NULL},
  {"checkpoint_sync", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 305,
    INT64_MIN, INT64_MAX, NULL},
  {"chunk_cache", "category", NULL, NULL, confchk_wiredtiger_open_chunk_cache_subconfigs, 9,
    confchk_wiredtiger_open_chunk_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 213,
    INT64_MIN, INT64_MAX, NULL},
  {"compatibility", "category", NULL, NULL, confchk_wiredtiger_open_compatibility_subconfigs, 3,
    confchk_wiredtiger_open_compatibility_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 215,
    INT64_MIN, INT64_MAX, NULL},
  {"compile_configuration_count", "int", NULL, "min=500", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 312, 500, INT64_MAX, NULL},
  {"debug_mode", "category", NULL, NULL, confchk_wiredtiger_open_debug_mode_subconfigs, 18,
    confchk_wiredtiger_open_debug_mode_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 217,
    INT64_MIN, INT64_MAX, NULL},
  {"direct_io", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 315, INT64_MIN,
    INT64_MAX, NULL},
  {"disaggregated", "category", NULL, NULL, confchk_wiredtiger_open_disaggregated_subconfigs, 6,
    confchk_wiredtiger_open_disaggregated_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 20,
    INT64_MIN, INT64_MAX, NULL},
  {"encryption", "category", NULL, NULL, confchk_wiredtiger_open_encryption_subconfigs, 3,
    confchk_wiredtiger_open_encryption_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 22,
    INT64_MIN, INT64_MAX, NULL},
  {"error_prefix", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, 236,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction", "category", NULL, NULL, confchk_wiredtiger_open_eviction_subconfigs, 5,
    confchk_wiredtiger_open_eviction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 237,
    INT64_MIN, INT64_MAX, NULL},
  {"eviction_checkpoint_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 243, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_target", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 244, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_dirty_trigger", "int", NULL, "min=1,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 245, 1, 10LL * WT_TERABYTE, NULL},
  {"eviction_target", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    246, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_trigger", "int", NULL, "min=10,max=10TB", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    247, 10, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_target", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 248, 0, 10LL * WT_TERABYTE, NULL},
  {"eviction_updates_trigger", "int", NULL, "min=0,max=10TB", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 249, 0, 10LL * WT_TERABYTE, NULL},
  {"extensions", "list", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 317, INT64_MIN,
    INT64_MAX, NULL},
  {"extra_diagnostics", "list", NULL,
    "choices=[\"all\",\"checkpoint_validate\",\"cursor_check\""
    ",\"disk_validate\",\"eviction_check\",\"generation_check\","
    "\"hs_validate\",\"key_out_of_order\",\"log_validate\","
    "\"prepared\",\"slow_operation\",\"txn_visibility\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 250, INT64_MIN, INT64_MAX,
    confchk_extra_diagnostics5_choices},
  {"file_extend", "list", NULL, "choices=[\"data\",\"log\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 318, INT64_MIN, INT64_MAX, confchk_file_extend4_choices},
  {"file_manager", "category", NULL, NULL, confchk_wiredtiger_open_file_manager_subconfigs, 3,
    confchk_wiredtiger_open_file_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 251,
    INT64_MIN, INT64_MAX, NULL},
  {"generation_drain_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    255, 0, INT64_MAX, NULL},
  {"hash", "category", NULL, NULL, confchk_wiredtiger_open_hash_subconfigs, 2,
    confchk_wiredtiger_open_hash_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 319, INT64_MIN,
    INT64_MAX, NULL},
  {"hazard_max", "int", NULL, "min=15", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 322, 15,
    INT64_MAX, NULL},
  {"heuristic_controls", "category", NULL, NULL,
    confchk_wiredtiger_open_heuristic_controls_subconfigs, 3,
    confchk_wiredtiger_open_heuristic_controls_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    256, INT64_MIN, INT64_MAX, NULL},
  {"history_store", "category", NULL, NULL, confchk_wiredtiger_open_history_store_subconfigs, 1,
    confchk_wiredtiger_open_history_store_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 260,
    INT64_MIN, INT64_MAX, NULL},
  {"io_capacity", "category", NULL, NULL, confchk_wiredtiger_open_io_capacity_subconfigs, 2,
    confchk_wiredtiger_open_io_capacity_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 262,
    INT64_MIN, INT64_MAX, NULL},
  {"json_output", "list", NULL, "choices=[\"error\",\"message\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 265, INT64_MIN, INT64_MAX, confchk_json_output5_choices},
  {"live_restore", "category", NULL, NULL, confchk_wiredtiger_open_live_restore_subconfigs, 4,
    confchk_wiredtiger_open_live_restore_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 64,
    INT64_MIN, INT64_MAX, NULL},
  {"log", "category", NULL, NULL, confchk_wiredtiger_open_log_subconfigs, 12,
    confchk_wiredtiger_open_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 40, INT64_MIN,
    INT64_MAX, NULL},
  {"mmap", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 327, INT64_MIN,
    INT64_MAX, NULL},
  {"mmap_all", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 328,
    INT64_MIN, INT64_MAX, NULL},
  {"multiprocess", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 329,
    INT64_MIN, INT64_MAX, NULL},
  {"operation_timeout_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 166, 0,
    INT64_MAX, NULL},
  {"operation_tracking", "category", NULL, NULL,
    confchk_wiredtiger_open_operation_tracking_subconfigs, 2,
    confchk_wiredtiger_open_operation_tracking_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    272, INT64_MIN, INT64_MAX, NULL},
  {"page_delta", "category", NULL, NULL, confchk_wiredtiger_open_page_delta_subconfigs, 5,
    confchk_wiredtiger_open_page_delta_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 274,
    INT64_MIN, INT64_MAX, NULL},
  {"precise_checkpoint", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 330,
    INT64_MIN, INT64_MAX, NULL},
  {"prefetch", "category", NULL, NULL, confchk_wiredtiger_open_prefetch_subconfigs, 2,
    confchk_wiredtiger_open_prefetch_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 300,
    INT64_MIN, INT64_MAX, NULL},
  {"preserve_prepared", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 333,
    INT64_MIN, INT64_MAX, NULL},
  {"readonly", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 67, INT64_MIN,
    INT64_MAX, NULL},
  {"rollback_to_stable", "category", NULL, NULL,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs, 1,
    confchk_wiredtiger_open_rollback_to_stable_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    280, INT64_MIN, INT64_MAX, NULL},
  {"salvage", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 334, INT64_MIN,
    INT64_MAX, NULL},
  {"session_max", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 335, 1,
    INT64_MAX, NULL},
  {"session_scratch_max", "int", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 336,
    INT64_MIN, INT64_MAX, NULL},
  {"session_table_cache", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    337, INT64_MIN, INT64_MAX, NULL},
  {"shared_cache", "category", NULL, NULL, confchk_wiredtiger_open_shared_cache_subconfigs, 5,
    confchk_wiredtiger_open_shared_cache_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 282,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics", "list", NULL,
    "choices=[\"all\",\"cache_walk\",\"fast\",\"none\","
    "\"clear\",\"tree_walk\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 147, INT64_MIN, INT64_MAX,
    confchk_statistics6_choices},
  {"statistics_log", "category", NULL, NULL, confchk_wiredtiger_open_statistics_log_subconfigs, 6,
    confchk_wiredtiger_open_statistics_log_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 286,
    INT64_MIN, INT64_MAX, NULL},
  {"tiered_storage", "category", NULL, NULL, confchk_tiered_storage_subconfigs, 8,
    confchk_tiered_storage_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, 51, INT64_MIN,
    INT64_MAX, NULL},
  {"timing_stress_for_test", "list", NULL,
    "choices=[\"aggressive_stash_free\",\"aggressive_sweep\","
    "\"backup_rename\",\"checkpoint_evict_page\","
    "\"checkpoint_handle\",\"checkpoint_slow\",\"checkpoint_stop\","
    "\"commit_transaction_slow\",\"compact_slow\","
    "\"conn_close_stress_log_printf\",\"evict_reposition\","
    "\"failpoint_eviction_split\","
    "\"failpoint_history_store_delete_key_from_ts\","
    "\"history_store_checkpoint_delay\",\"history_store_search\","
    "\"history_store_sweep_race\",\"live_restore_clean_up\","
    "\"open_index_slow\",\"prefetch_1\",\"prefetch_2\",\"prefetch_3\""
    ",\"prefix_compare\",\"prepare_checkpoint_delay\","
    "\"prepare_resolution_1\",\"prepare_resolution_2\","
    "\"session_alter_slow\",\"sleep_before_read_overflow_onpage\","
    "\"split_1\",\"split_2\",\"split_3\",\"split_4\",\"split_5\","
    "\"split_6\",\"split_7\",\"split_8\",\"tiered_flush_finish\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 291, INT64_MIN, INT64_MAX,
    confchk_timing_stress_for_test5_choices},
  {"transaction_sync", "category", NULL, NULL, confchk_wiredtiger_open_transaction_sync_subconfigs,
    2, confchk_wiredtiger_open_transaction_sync_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY,
    339, INT64_MIN, INT64_MAX, NULL},
  {"verbose", "list", NULL,
    "choices=[\"all\",\"api\",\"backup\",\"block\","
    "\"block_cache\",\"checkpoint\",\"checkpoint_cleanup\","
    "\"checkpoint_progress\",\"chunkcache\",\"compact\","
    "\"compact_progress\",\"configuration\",\"disaggregated_storage\""
    ",\"error_returns\",\"eviction\",\"fileops\",\"generation\","
    "\"handleops\",\"history_store\",\"history_store_activity\","
    "\"layered\",\"live_restore\",\"live_restore_progress\",\"log\","
    "\"metadata\",\"mutex\",\"out_of_order\",\"overflow\","
    "\"page_delta\",\"prefetch\",\"read\",\"reconcile\",\"recovery\","
    "\"recovery_progress\",\"rts\",\"salvage\",\"shared_cache\","
    "\"split\",\"sweep\",\"temporary\",\"thread_group\",\"tiered\","
    "\"timestamp\",\"transaction\",\"verify\",\"version\",\"write\"]",
    NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_LIST, 10, INT64_MIN, INT64_MAX,
    confchk_verbose16_choices},
  {"verify_metadata", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, 342,
    INT64_MIN, INT64_MAX, NULL},
  {"write_through", "list", NULL, "choices=[\"data\",\"log\"]", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_LIST, 343, INT64_MIN, INT64_MAX, confchk_write_through4_choices},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL}};

static const uint8_t confchk_wiredtiger_open_usercfg_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 16, 19,
  31, 33, 34, 38, 39, 40, 40, 42, 45, 45, 47, 51, 51, 53, 60, 63, 63, 65, 66, 66, 66, 66, 66, 66,
  66, 66};

static const WT_CONFIG_ENTRY config_entries[] = {
  {"WT_CONNECTION.add_collator", "", NULL, 0, NULL, 0, WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.add_compressor", "", NULL, 0, NULL, 1, WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.add_data_source", "", NULL, 0, NULL, 2, WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.add_encryptor", "", NULL, 0, NULL, 3, WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.add_page_log", "", NULL, 0, NULL, 4, WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.add_storage_source", "", NULL, 0, NULL, 5, WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.close", "final_flush=false,leak_memory=false,use_timestamp=true",
    confchk_WT_CONNECTION_close, 3, confchk_WT_CONNECTION_close_jump, 6, WT_CONF_SIZING_NONE,
    false},
  {"WT_CONNECTION.debug_info",
    "backup=false,cache=false,cursors=false,handles=false,log=false,"
    "metadata=false,sessions=false,txn=false",
    confchk_WT_CONNECTION_debug_info, 8, confchk_WT_CONNECTION_debug_info_jump, 7,
    WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.load_extension",
    "config=,early_load=false,entry=wiredtiger_extension_init,"
    "terminate=wiredtiger_extension_terminate",
    confchk_WT_CONNECTION_load_extension, 4, confchk_WT_CONNECTION_load_extension_jump, 8,
    WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.open_session",
    "cache_cursors=true,cache_max_wait_ms=0,"
    "debug=(checkpoint_fail_before_turtle_update=false,"
    "release_evict_page=false),ignore_cache_size=false,"
    "isolation=snapshot,prefetch=(enabled=false)",
    confchk_WT_CONNECTION_open_session, 6, confchk_WT_CONNECTION_open_session_jump, 9,
    WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.query_timestamp", "get=all_durable", confchk_WT_CONNECTION_query_timestamp, 1,
    confchk_WT_CONNECTION_query_timestamp_jump, 10, WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.reconfigure",
    "block_cache=(blkcache_eviction_aggression=1800,"
    "cache_on_checkpoint=true,cache_on_writes=true,enabled=false,"
    "full_target=95,hashsize=32768,max_percent_overhead=10,"
    "nvram_path=,percent_file_in_dram=50,size=0,system_ram=0,type=),"
    "cache_eviction_controls=(incremental_app_eviction=false,"
    "scrub_evict_under_target_limit=true),cache_max_wait_ms=0,"
    "cache_overhead=8,cache_size=100MB,cache_stuck_timeout_ms=300000,"
    "checkpoint=(log_size=0,wait=0),"
    "checkpoint_cleanup=(file_wait_ms=0,method=none,wait=300),"
    "chunk_cache=(pinned=),compatibility=(release=),"
    "debug_mode=(background_compact=false,checkpoint_retention=0,"
    "configuration=false,corruption_abort=true,cursor_copy=false,"
    "cursor_reposition=false,eviction=false,"
    "eviction_checkpoint_ts_ordering=false,log_retention=0,"
    "page_history=false,realloc_exact=false,realloc_malloc=false,"
    "rollback_error=0,slow_checkpoint=false,stress_skiplist=false,"
    "table_logging=false,tiered_flush_error_continue=false,"
    "update_restore_evict=false),disaggregated=(checkpoint_meta=,"
    "last_materialized_lsn=,local_files_action=delete,"
    "lose_all_my_data=false,role=),error_prefix=,"
    "eviction=(evict_sample_inmem=true,evict_use_softptr=false,"
    "legacy_page_visit_strategy=false,threads_max=8,threads_min=1),"
    "eviction_checkpoint_target=1,eviction_dirty_target=5,"
    "eviction_dirty_trigger=20,eviction_target=80,eviction_trigger=95"
    ",eviction_updates_target=0,eviction_updates_trigger=0,"
    "extra_diagnostics=[],file_manager=(close_handle_minimum=250,"
    "close_idle_time=30,close_scan_interval=10),"
    "generation_drain_timeout_ms=240000,"
    "heuristic_controls=(checkpoint_cleanup_obsolete_tw_pages_dirty_max=100"
    ",eviction_obsolete_tw_pages_dirty_max=100,"
    "obsolete_tw_btree_max=100),history_store=(file_max=0),"
    "io_capacity=(chunk_cache=0,total=0),json_output=[],"
    "log=(archive=true,os_cache_dirty_pct=0,prealloc=true,"
    "prealloc_init_count=1,remove=true,zero_fill=false),"
    "operation_timeout_ms=0,operation_tracking=(enabled=false,"
    "path=\".\"),page_delta=(delta_pct=20,"
    "flatten_leaf_page_delta=false,internal_page_delta=true,"
    "leaf_page_delta=true,max_consecutive_delta=32),"
    "rollback_to_stable=(threads=4),shared_cache=(chunk=10MB,name=,"
    "quota=0,reserve=0,size=500MB),statistics=none,"
    "statistics_log=(json=false,on_close=false,sources=,"
    "timestamp=\"%b %d %H:%M:%S\",wait=0),"
    "tiered_storage=(local_retention=300),timing_stress_for_test=,"
    "verbose=[]",
    confchk_WT_CONNECTION_reconfigure, 39, confchk_WT_CONNECTION_reconfigure_jump, 11,
    WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.rollback_to_stable", "dryrun=false,threads=4",
    confchk_WT_CONNECTION_rollback_to_stable, 2, confchk_WT_CONNECTION_rollback_to_stable_jump, 12,
    WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.set_file_system", "", NULL, 0, NULL, 13, WT_CONF_SIZING_NONE, false},
  {"WT_CONNECTION.set_timestamp",
    "durable_timestamp=,force=false,oldest_timestamp=,"
    "stable_timestamp=",
    confchk_WT_CONNECTION_set_timestamp, 4, confchk_WT_CONNECTION_set_timestamp_jump, 14,
    WT_CONF_SIZING_NONE, false},
  {"WT_CURSOR.bound", "action=set,bound=,inclusive=true", confchk_WT_CURSOR_bound, 3,
    confchk_WT_CURSOR_bound_jump, 15, WT_CONF_SIZING_INITIALIZE(WT_CURSOR, bound), true},
  {"WT_CURSOR.close", "", NULL, 0, NULL, 16, WT_CONF_SIZING_NONE, false},
  {"WT_CURSOR.reconfigure", "append=false,overwrite=true,prefix_search=false",
    confchk_WT_CURSOR_reconfigure, 3, confchk_WT_CURSOR_reconfigure_jump, 17, WT_CONF_SIZING_NONE,
    false},
  {"WT_SESSION.alter",
    "access_pattern_hint=none,app_metadata=,"
    "assert=(commit_timestamp=none,durable_timestamp=none,"
    "read_timestamp=none,write_timestamp=off),cache_resident=false,"
    "checkpoint=,exclusive_refreshed=true,log=(enabled=true),"
    "os_cache_dirty_max=0,os_cache_max=0,verbose=[],"
    "write_timestamp_usage=none",
    confchk_WT_SESSION_alter, 11, confchk_WT_SESSION_alter_jump, 18, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.begin_transaction",
    "claim_prepared_id=,ignore_prepare=false,isolation=,name=,"
    "no_timestamp=false,operation_timeout_ms=0,priority=0,"
    "read_timestamp=,roundup_timestamps=(prepared=false,read=false),"
    "sync=",
    confchk_WT_SESSION_begin_transaction, 10, confchk_WT_SESSION_begin_transaction_jump, 19,
    WT_CONF_SIZING_INITIALIZE(WT_SESSION, begin_transaction), true},
  {"WT_SESSION.checkpoint",
    "debug=(checkpoint_cleanup=false,checkpoint_crash_point=-1),drop="
    ",flush_tier=(enabled=false,force=false,sync=true,timeout=0),"
    "force=false,name=,use_timestamp=true",
    confchk_WT_SESSION_checkpoint, 6, confchk_WT_SESSION_checkpoint_jump, 20, WT_CONF_SIZING_NONE,
    false},
  {"WT_SESSION.close", "", NULL, 0, NULL, 21, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.commit_transaction",
    "commit_timestamp=,durable_timestamp=,operation_timeout_ms=0,"
    "sync=",
    confchk_WT_SESSION_commit_transaction, 4, confchk_WT_SESSION_commit_transaction_jump, 22,
    WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.compact",
    "background=,dryrun=false,exclude=,free_space_target=20MB,"
    "run_once=false,timeout=1200",
    confchk_WT_SESSION_compact, 6, confchk_WT_SESSION_compact_jump, 23, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.create",
    "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
    "assert=(commit_timestamp=none,durable_timestamp=none,"
    "read_timestamp=none,write_timestamp=off),block_allocation=best,"
    "block_compressor=,block_manager=default,cache_resident=false,"
    "checksum=on,colgroups=,collator=,columns=,dictionary=0,"
    "disaggregated=(page_log=),encryption=(keyid=,name=),"
    "exclusive=false,extractor=,format=btree,huffman_key=,"
    "huffman_value=,ignore_in_memory_cache_size=false,immutable=false"
    ",import=(compare_timestamp=oldest_timestamp,enabled=false,"
    "file_metadata=,metadata_file=,panic_corrupt=true,repair=false),"
    "in_memory=false,ingest=,internal_item_max=0,internal_key_max=0,"
    "internal_key_truncate=true,internal_page_max=4KB,key_format=u,"
    "key_gap=10,leaf_item_max=0,leaf_key_max=0,leaf_page_max=32KB,"
    "leaf_value_max=0,log=(enabled=true),lsm=(auto_throttle=,bloom=,"
    "bloom_bit_count=,bloom_config=,bloom_hash_count=,bloom_oldest=,"
    "chunk_count_limit=,chunk_max=,chunk_size=,merge_max=,merge_min=)"
    ",memory_page_image_max=0,memory_page_max=5MB,"
    "os_cache_dirty_max=0,os_cache_max=0,prefix_compression=false,"
    "prefix_compression_min=4,source=,split_deepen_min_child=0,"
    "split_deepen_per_child=0,split_pct=90,stable=,"
    "tiered_storage=(auth_token=,bucket=,bucket_prefix=,"
    "cache_directory=,local_retention=300,name=,object_target_size=0,"
    "shared=false),type=file,value_format=u,verbose=[],"
    "write_timestamp_usage=none",
    confchk_WT_SESSION_create, 53, confchk_WT_SESSION_create_jump, 24, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.drop",
    "checkpoint_wait=true,force=false,lock_wait=true,"
    "remove_files=true,remove_shared=false",
    confchk_WT_SESSION_drop, 5, confchk_WT_SESSION_drop_jump, 25, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.log_flush", "sync=on", confchk_WT_SESSION_log_flush, 1,
    confchk_WT_SESSION_log_flush_jump, 26, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.log_printf", "", NULL, 0, NULL, 27, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.open_cursor",
    "append=false,bulk=false,checkpoint=,checkpoint_use_history=true,"
    "checkpoint_wait=true,debug=(checkpoint_read_timestamp=,"
    "dump_version=false,release_evict=false),dump=,"
    "incremental=(consolidate=false,enabled=false,file=,"
    "force_stop=false,granularity=16MB,src_id=,this_id=),"
    "next_random=false,next_random_sample_size=0,next_random_seed=0,"
    "overwrite=true,prefix_search=false,raw=false,read_once=false,"
    "readonly=false,skip_sort_check=false,statistics=,target=",
    confchk_WT_SESSION_open_cursor, 19, confchk_WT_SESSION_open_cursor_jump, 28,
    WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.prepare_transaction",
    "prepare_timestamp=,prepared_id=", confchk_WT_SESSION_prepare_transaction, 2,
    confchk_WT_SESSION_prepare_transaction_jump, 29, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.prepared_id_transaction", "prepared_id=", confchk_WT_SESSION_prepared_id_transaction,
    1, confchk_WT_SESSION_prepared_id_transaction_jump, 30, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.prepared_id_transaction_uint", "", NULL, 0, NULL, 31, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.query_timestamp", "get=read", confchk_WT_SESSION_query_timestamp, 1,
    confchk_WT_SESSION_query_timestamp_jump, 32, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.reconfigure",
    "cache_cursors=true,cache_max_wait_ms=0,"
    "debug=(checkpoint_fail_before_turtle_update=false,"
    "release_evict_page=false),ignore_cache_size=false,"
    "isolation=snapshot,prefetch=(enabled=false)",
    confchk_WT_SESSION_reconfigure, 6, confchk_WT_SESSION_reconfigure_jump, 33, WT_CONF_SIZING_NONE,
    false},
  {"WT_SESSION.reset", "", NULL, 0, NULL, 34, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.reset_snapshot", "", NULL, 0, NULL, 35, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.rollback_transaction",
    "operation_timeout_ms=0,rollback_timestamp=", confchk_WT_SESSION_rollback_transaction, 2,
    confchk_WT_SESSION_rollback_transaction_jump, 36, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.salvage", "force=false", confchk_WT_SESSION_salvage, 1,
    confchk_WT_SESSION_salvage_jump, 37, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.strerror", "", NULL, 0, NULL, 38, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.timestamp_transaction",
    "commit_timestamp=,durable_timestamp=,prepare_timestamp=,"
    "read_timestamp=,rollback_timestamp=",
    confchk_WT_SESSION_timestamp_transaction, 5, confchk_WT_SESSION_timestamp_transaction_jump, 39,
    WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.timestamp_transaction_uint", "", NULL, 0, NULL, 40, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.truncate", "", NULL, 0, NULL, 41, WT_CONF_SIZING_NONE, false},
  {"WT_SESSION.verify",
    "do_not_clear_txn_id=false,dump_address=false,dump_all_data=false"
    ",dump_blocks=false,dump_key_data=false,dump_layout=false,"
    "dump_offsets=,dump_pages=false,dump_tree_shape=false,"
    "read_corrupt=false,stable_timestamp=false,strict=false",
    confchk_WT_SESSION_verify, 12, confchk_WT_SESSION_verify_jump, 42, WT_CONF_SIZING_NONE, false},
  {"colgroup.meta",
    "app_metadata=,assert=(commit_timestamp=none,"
    "durable_timestamp=none,read_timestamp=none,write_timestamp=off),"
    "collator=,columns=,source=,type=file,verbose=[],"
    "write_timestamp_usage=none",
    confchk_colgroup_meta, 8, confchk_colgroup_meta_jump, 43, WT_CONF_SIZING_NONE, false},
  {"file.config",
    "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
    "assert=(commit_timestamp=none,durable_timestamp=none,"
    "read_timestamp=none,write_timestamp=off),block_allocation=best,"
    "block_compressor=,block_manager=default,cache_resident=false,"
    "checksum=on,collator=,columns=,dictionary=0,"
    "disaggregated=(page_log=),encryption=(keyid=,name=),format=btree"
    ",huffman_key=,huffman_value=,ignore_in_memory_cache_size=false,"
    "in_memory=false,internal_item_max=0,internal_key_max=0,"
    "internal_key_truncate=true,internal_page_max=4KB,key_format=u,"
    "key_gap=10,leaf_item_max=0,leaf_key_max=0,leaf_page_max=32KB,"
    "leaf_value_max=0,log=(enabled=true),memory_page_image_max=0,"
    "memory_page_max=5MB,os_cache_dirty_max=0,os_cache_max=0,"
    "prefix_compression=false,prefix_compression_min=4,"
    "split_deepen_min_child=0,split_deepen_per_child=0,split_pct=90,"
    "tiered_storage=(auth_token=,bucket=,bucket_prefix=,"
    "cache_directory=,local_retention=300,name=,object_target_size=0,"
    "shared=false),value_format=u,verbose=[],"
    "write_timestamp_usage=none",
    confchk_file_config, 43, confchk_file_config_jump, 44, WT_CONF_SIZING_NONE, false},
  {"file.meta",
    "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
    "assert=(commit_timestamp=none,durable_timestamp=none,"
    "read_timestamp=none,write_timestamp=off),block_allocation=best,"
    "block_compressor=,block_manager=default,cache_resident=false,"
    "checkpoint=,checkpoint_backup_info=,checkpoint_lsn=,checksum=on,"
    "collator=,columns=,dictionary=0,disaggregated=(page_log=),"
    "encryption=(keyid=,name=),format=btree,huffman_key=,"
    "huffman_value=,id=,ignore_in_memory_cache_size=false,"
    "in_memory=false,internal_item_max=0,internal_key_max=0,"
    "internal_key_truncate=true,internal_page_max=4KB,key_format=u,"
    "key_gap=10,leaf_item_max=0,leaf_key_max=0,leaf_page_max=32KB,"
    "leaf_value_max=0,live_restore=(bitmap=,nbits=0),"
    "log=(enabled=true),memory_page_image_max=0,memory_page_max=5MB,"
    "os_cache_dirty_max=0,os_cache_max=0,prefix_compression=false,"
    "prefix_compression_min=4,readonly=false,split_deepen_min_child=0"
    ",split_deepen_per_child=0,split_pct=90,tiered_object=false,"
    "tiered_storage=(auth_token=,bucket=,bucket_prefix=,"
    "cache_directory=,local_retention=300,name=,object_target_size=0,"
    "shared=false),value_format=u,verbose=[],version=(major=0,"
    "minor=0),write_timestamp_usage=none",
    confchk_file_meta, 51, confchk_file_meta_jump, 45, WT_CONF_SIZING_NONE, false},
  {"index.meta",
    "app_metadata=,assert=(commit_timestamp=none,"
    "durable_timestamp=none,read_timestamp=none,write_timestamp=off),"
    "collator=,columns=,extractor=,immutable=false,key_format=u,"
    "source=,type=file,value_format=u,verbose=[],"
    "write_timestamp_usage=none",
    confchk_index_meta, 12, confchk_index_meta_jump, 46, WT_CONF_SIZING_NONE, false},
  {"layered.meta",
    "app_metadata=,assert=(commit_timestamp=none,"
    "durable_timestamp=none,read_timestamp=none,write_timestamp=off),"
    "collator=,columns=,disaggregated=(checkpoint_meta=,"
    "last_materialized_lsn=,local_files_action=delete,"
    "lose_all_my_data=false,page_log=,role=),ingest=,key_format=u,"
    "log=(enabled=true),stable=,value_format=u,verbose=[],"
    "write_timestamp_usage=none",
    confchk_layered_meta, 12, confchk_layered_meta_jump, 47, WT_CONF_SIZING_NONE, false},
  {"object.meta",
    "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
    "assert=(commit_timestamp=none,durable_timestamp=none,"
    "read_timestamp=none,write_timestamp=off),block_allocation=best,"
    "block_compressor=,block_manager=default,cache_resident=false,"
    "checkpoint=,checkpoint_backup_info=,checkpoint_lsn=,checksum=on,"
    "collator=,columns=,dictionary=0,disaggregated=(page_log=),"
    "encryption=(keyid=,name=),flush_time=0,flush_timestamp=0,"
    "format=btree,huffman_key=,huffman_value=,id=,"
    "ignore_in_memory_cache_size=false,in_memory=false,"
    "internal_item_max=0,internal_key_max=0,"
    "internal_key_truncate=true,internal_page_max=4KB,key_format=u,"
    "key_gap=10,leaf_item_max=0,leaf_key_max=0,leaf_page_max=32KB,"
    "leaf_value_max=0,live_restore=(bitmap=,nbits=0),"
    "log=(enabled=true),memory_page_image_max=0,memory_page_max=5MB,"
    "os_cache_dirty_max=0,os_cache_max=0,prefix_compression=false,"
    "prefix_compression_min=4,readonly=false,split_deepen_min_child=0"
    ",split_deepen_per_child=0,split_pct=90,tiered_object=false,"
    "tiered_storage=(auth_token=,bucket=,bucket_prefix=,"
    "cache_directory=,local_retention=300,name=,object_target_size=0,"
    "shared=false),value_format=u,verbose=[],version=(major=0,"
    "minor=0),write_timestamp_usage=none",
    confchk_object_meta, 53, confchk_object_meta_jump, 48, WT_CONF_SIZING_NONE, false},
  {"table.meta",
    "app_metadata=,assert=(commit_timestamp=none,"
    "durable_timestamp=none,read_timestamp=none,write_timestamp=off),"
    "colgroups=,collator=,columns=,key_format=u,value_format=u,"
    "verbose=[],write_timestamp_usage=none",
    confchk_table_meta, 9, confchk_table_meta_jump, 49, WT_CONF_SIZING_NONE, false},
  {"tier.meta",
    "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
    "assert=(commit_timestamp=none,durable_timestamp=none,"
    "read_timestamp=none,write_timestamp=off),block_allocation=best,"
    "block_compressor=,block_manager=default,bucket=,bucket_prefix=,"
    "cache_directory=,cache_resident=false,checkpoint=,"
    "checkpoint_backup_info=,checkpoint_lsn=,checksum=on,collator=,"
    "columns=,dictionary=0,disaggregated=(page_log=),"
    "encryption=(keyid=,name=),format=btree,huffman_key=,"
    "huffman_value=,id=,ignore_in_memory_cache_size=false,"
    "in_memory=false,internal_item_max=0,internal_key_max=0,"
    "internal_key_truncate=true,internal_page_max=4KB,key_format=u,"
    "key_gap=10,leaf_item_max=0,leaf_key_max=0,leaf_page_max=32KB,"
    "leaf_value_max=0,live_restore=(bitmap=,nbits=0),"
    "log=(enabled=true),memory_page_image_max=0,memory_page_max=5MB,"
    "os_cache_dirty_max=0,os_cache_max=0,prefix_compression=false,"
    "prefix_compression_min=4,readonly=false,split_deepen_min_child=0"
    ",split_deepen_per_child=0,split_pct=90,tiered_object=false,"
    "tiered_storage=(auth_token=,bucket=,bucket_prefix=,"
    "cache_directory=,local_retention=300,name=,object_target_size=0,"
    "shared=false),value_format=u,verbose=[],version=(major=0,"
    "minor=0),write_timestamp_usage=none",
    confchk_tier_meta, 54, confchk_tier_meta_jump, 50, WT_CONF_SIZING_NONE, false},
  {"tiered.meta",
    "access_pattern_hint=none,allocation_size=4KB,app_metadata=,"
    "assert=(commit_timestamp=none,durable_timestamp=none,"
    "read_timestamp=none,write_timestamp=off),block_allocation=best,"
    "block_compressor=,block_manager=default,cache_resident=false,"
    "checkpoint=,checkpoint_backup_info=,checkpoint_lsn=,checksum=on,"
    "collator=,columns=,dictionary=0,disaggregated=(page_log=),"
    "encryption=(keyid=,name=),flush_time=0,flush_timestamp=0,"
    "format=btree,huffman_key=,huffman_value=,id=,"
    "ignore_in_memory_cache_size=false,in_memory=false,"
    "internal_item_max=0,internal_key_max=0,"
    "internal_key_truncate=true,internal_page_max=4KB,key_format=u,"
    "key_gap=10,last=0,leaf_item_max=0,leaf_key_max=0,"
    "leaf_page_max=32KB,leaf_value_max=0,live_restore=(bitmap=,"
    "nbits=0),log=(enabled=true),memory_page_image_max=0,"
    "memory_page_max=5MB,oldest=1,os_cache_dirty_max=0,os_cache_max=0"
    ",prefix_compression=false,prefix_compression_min=4,"
    "readonly=false,split_deepen_min_child=0,split_deepen_per_child=0"
    ",split_pct=90,tiered_object=false,tiered_storage=(auth_token=,"
    "bucket=,bucket_prefix=,cache_directory=,local_retention=300,"
    "name=,object_target_size=0,shared=false),tiers=,value_format=u,"
    "verbose=[],version=(major=0,minor=0),write_timestamp_usage=none",
    confchk_tiered_meta, 56, confchk_tiered_meta_jump, 51, WT_CONF_SIZING_NONE, false},
  {"wiredtiger_open",
    "backup_restore_target=,"
    "block_cache=(blkcache_eviction_aggression=1800,"
    "cache_on_checkpoint=true,cache_on_writes=true,enabled=false,"
    "full_target=95,hashsize=32768,max_percent_overhead=10,"
    "nvram_path=,percent_file_in_dram=50,size=0,system_ram=0,type=),"
    "buffer_alignment=,builtin_extension_config=,cache_cursors=true,"
    "cache_eviction_controls=(incremental_app_eviction=false,"
    "scrub_evict_under_target_limit=true),cache_max_wait_ms=0,"
    "cache_overhead=8,cache_size=100MB,cache_stuck_timeout_ms=300000,"
    "checkpoint=(log_size=0,wait=0),"
    "checkpoint_cleanup=(file_wait_ms=0,method=none,wait=300),"
    "checkpoint_sync=true,chunk_cache=(capacity=10GB,"
    "chunk_cache_evict_trigger=90,chunk_size=1MB,enabled=false,"
    "flushed_data_cache_insertion=true,hashsize=1024,pinned=,"
    "storage_path=,type=FILE),compatibility=(release=,require_max=,"
    "require_min=),compile_configuration_count=1000,config_base=true,"
    "create=false,debug_mode=(background_compact=false,"
    "checkpoint_retention=0,configuration=false,corruption_abort=true"
    ",cursor_copy=false,cursor_reposition=false,eviction=false,"
    "eviction_checkpoint_ts_ordering=false,log_retention=0,"
    "page_history=false,realloc_exact=false,realloc_malloc=false,"
    "rollback_error=0,slow_checkpoint=false,stress_skiplist=false,"
    "table_logging=false,tiered_flush_error_continue=false,"
    "update_restore_evict=false),direct_io=,"
    "disaggregated=(checkpoint_meta=,last_materialized_lsn=,"
    "local_files_action=delete,lose_all_my_data=false,page_log=,"
    "role=),encryption=(keyid=,name=,secretkey=),error_prefix=,"
    "eviction=(evict_sample_inmem=true,evict_use_softptr=false,"
    "legacy_page_visit_strategy=false,threads_max=8,threads_min=1),"
    "eviction_checkpoint_target=1,eviction_dirty_target=5,"
    "eviction_dirty_trigger=20,eviction_target=80,eviction_trigger=95"
    ",eviction_updates_target=0,eviction_updates_trigger=0,"
    "exclusive=false,extensions=,extra_diagnostics=[],file_extend=,"
    "file_manager=(close_handle_minimum=250,close_idle_time=30,"
    "close_scan_interval=10),generation_drain_timeout_ms=240000,"
    "hash=(buckets=512,dhandle_buckets=512),hazard_max=1000,"
    "heuristic_controls=(checkpoint_cleanup_obsolete_tw_pages_dirty_max=100"
    ",eviction_obsolete_tw_pages_dirty_max=100,"
    "obsolete_tw_btree_max=100),history_store=(file_max=0),"
    "in_memory=false,io_capacity=(chunk_cache=0,total=0),"
    "json_output=[],live_restore=(enabled=false,path=,read_size=1MB,"
    "threads_max=8),log=(archive=true,compressor=,enabled=false,"
    "file_max=100MB,force_write_wait=0,os_cache_dirty_pct=0,"
    "path=\".\",prealloc=true,prealloc_init_count=1,recover=on,"
    "remove=true,zero_fill=false),mmap=true,mmap_all=false,"
    "multiprocess=false,operation_timeout_ms=0,"
    "operation_tracking=(enabled=false,path=\".\"),"
    "page_delta=(delta_pct=20,flatten_leaf_page_delta=false,"
    "internal_page_delta=true,leaf_page_delta=true,"
    "max_consecutive_delta=32),precise_checkpoint=false,"
    "prefetch=(available=false,default=false),preserve_prepared=false"
    ",readonly=false,rollback_to_stable=(threads=4),salvage=false,"
    "session_max=100,session_scratch_max=2MB,session_table_cache=true"
    ",shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
    "statistics=none,statistics_log=(json=false,on_close=false,"
    "path=\".\",sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
    "tiered_storage=(auth_token=,bucket=,bucket_prefix=,"
    "cache_directory=,interval=60,local_retention=300,name=,"
    "shared=false),timing_stress_for_test=,"
    "transaction_sync=(enabled=false,method=fsync),"
    "use_environment=true,use_environment_priv=false,verbose=[],"
    "verify_metadata=false,write_through=",
    confchk_wiredtiger_open, 72, confchk_wiredtiger_open_jump, 52, WT_CONF_SIZING_NONE, false},
  {"wiredtiger_open_all",
    "backup_restore_target=,"
    "block_cache=(blkcache_eviction_aggression=1800,"
    "cache_on_checkpoint=true,cache_on_writes=true,enabled=false,"
    "full_target=95,hashsize=32768,max_percent_overhead=10,"
    "nvram_path=,percent_file_in_dram=50,size=0,system_ram=0,type=),"
    "buffer_alignment=,builtin_extension_config=,cache_cursors=true,"
    "cache_eviction_controls=(incremental_app_eviction=false,"
    "scrub_evict_under_target_limit=true),cache_max_wait_ms=0,"
    "cache_overhead=8,cache_size=100MB,cache_stuck_timeout_ms=300000,"
    "checkpoint=(log_size=0,wait=0),"
    "checkpoint_cleanup=(file_wait_ms=0,method=none,wait=300),"
    "checkpoint_sync=true,chunk_cache=(capacity=10GB,"
    "chunk_cache_evict_trigger=90,chunk_size=1MB,enabled=false,"
    "flushed_data_cache_insertion=true,hashsize=1024,pinned=,"
    "storage_path=,type=FILE),compatibility=(release=,require_max=,"
    "require_min=),compile_configuration_count=1000,config_base=true,"
    "create=false,debug_mode=(background_compact=false,"
    "checkpoint_retention=0,configuration=false,corruption_abort=true"
    ",cursor_copy=false,cursor_reposition=false,eviction=false,"
    "eviction_checkpoint_ts_ordering=false,log_retention=0,"
    "page_history=false,realloc_exact=false,realloc_malloc=false,"
    "rollback_error=0,slow_checkpoint=false,stress_skiplist=false,"
    "table_logging=false,tiered_flush_error_continue=false,"
    "update_restore_evict=false),direct_io=,"
    "disaggregated=(checkpoint_meta=,last_materialized_lsn=,"
    "local_files_action=delete,lose_all_my_data=false,page_log=,"
    "role=),encryption=(keyid=,name=,secretkey=),error_prefix=,"
    "eviction=(evict_sample_inmem=true,evict_use_softptr=false,"
    "legacy_page_visit_strategy=false,threads_max=8,threads_min=1),"
    "eviction_checkpoint_target=1,eviction_dirty_target=5,"
    "eviction_dirty_trigger=20,eviction_target=80,eviction_trigger=95"
    ",eviction_updates_target=0,eviction_updates_trigger=0,"
    "exclusive=false,extensions=,extra_diagnostics=[],file_extend=,"
    "file_manager=(close_handle_minimum=250,close_idle_time=30,"
    "close_scan_interval=10),generation_drain_timeout_ms=240000,"
    "hash=(buckets=512,dhandle_buckets=512),hazard_max=1000,"
    "heuristic_controls=(checkpoint_cleanup_obsolete_tw_pages_dirty_max=100"
    ",eviction_obsolete_tw_pages_dirty_max=100,"
    "obsolete_tw_btree_max=100),history_store=(file_max=0),"
    "in_memory=false,io_capacity=(chunk_cache=0,total=0),"
    "json_output=[],live_restore=(enabled=false,path=,read_size=1MB,"
    "threads_max=8),log=(archive=true,compressor=,enabled=false,"
    "file_max=100MB,force_write_wait=0,os_cache_dirty_pct=0,"
    "path=\".\",prealloc=true,prealloc_init_count=1,recover=on,"
    "remove=true,zero_fill=false),mmap=true,mmap_all=false,"
    "multiprocess=false,operation_timeout_ms=0,"
    "operation_tracking=(enabled=false,path=\".\"),"
    "page_delta=(delta_pct=20,flatten_leaf_page_delta=false,"
    "internal_page_delta=true,leaf_page_delta=true,"
    "max_consecutive_delta=32),precise_checkpoint=false,"
    "prefetch=(available=false,default=false),preserve_prepared=false"
    ",readonly=false,rollback_to_stable=(threads=4),salvage=false,"
    "session_max=100,session_scratch_max=2MB,session_table_cache=true"
    ",shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
    "statistics=none,statistics_log=(json=false,on_close=false,"
    "path=\".\",sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
    "tiered_storage=(auth_token=,bucket=,bucket_prefix=,"
    "cache_directory=,interval=60,local_retention=300,name=,"
    "shared=false),timing_stress_for_test=,"
    "transaction_sync=(enabled=false,method=fsync),"
    "use_environment=true,use_environment_priv=false,verbose=[],"
    "verify_metadata=false,version=(major=0,minor=0),write_through=",
    confchk_wiredtiger_open_all, 73, confchk_wiredtiger_open_all_jump, 53, WT_CONF_SIZING_NONE,
    false},
  {"wiredtiger_open_basecfg",
    "backup_restore_target=,"
    "block_cache=(blkcache_eviction_aggression=1800,"
    "cache_on_checkpoint=true,cache_on_writes=true,enabled=false,"
    "full_target=95,hashsize=32768,max_percent_overhead=10,"
    "nvram_path=,percent_file_in_dram=50,size=0,system_ram=0,type=),"
    "buffer_alignment=,builtin_extension_config=,cache_cursors=true,"
    "cache_eviction_controls=(incremental_app_eviction=false,"
    "scrub_evict_under_target_limit=true),cache_max_wait_ms=0,"
    "cache_overhead=8,cache_size=100MB,cache_stuck_timeout_ms=300000,"
    "checkpoint=(log_size=0,wait=0),"
    "checkpoint_cleanup=(file_wait_ms=0,method=none,wait=300),"
    "checkpoint_sync=true,chunk_cache=(capacity=10GB,"
    "chunk_cache_evict_trigger=90,chunk_size=1MB,enabled=false,"
    "flushed_data_cache_insertion=true,hashsize=1024,pinned=,"
    "storage_path=,type=FILE),compatibility=(release=,require_max=,"
    "require_min=),compile_configuration_count=1000,"
    "debug_mode=(background_compact=false,checkpoint_retention=0,"
    "configuration=false,corruption_abort=true,cursor_copy=false,"
    "cursor_reposition=false,eviction=false,"
    "eviction_checkpoint_ts_ordering=false,log_retention=0,"
    "page_history=false,realloc_exact=false,realloc_malloc=false,"
    "rollback_error=0,slow_checkpoint=false,stress_skiplist=false,"
    "table_logging=false,tiered_flush_error_continue=false,"
    "update_restore_evict=false),direct_io=,"
    "disaggregated=(checkpoint_meta=,last_materialized_lsn=,"
    "local_files_action=delete,lose_all_my_data=false,page_log=,"
    "role=),encryption=(keyid=,name=,secretkey=),error_prefix=,"
    "eviction=(evict_sample_inmem=true,evict_use_softptr=false,"
    "legacy_page_visit_strategy=false,threads_max=8,threads_min=1),"
    "eviction_checkpoint_target=1,eviction_dirty_target=5,"
    "eviction_dirty_trigger=20,eviction_target=80,eviction_trigger=95"
    ",eviction_updates_target=0,eviction_updates_trigger=0,"
    "extensions=,extra_diagnostics=[],file_extend=,"
    "file_manager=(close_handle_minimum=250,close_idle_time=30,"
    "close_scan_interval=10),generation_drain_timeout_ms=240000,"
    "hash=(buckets=512,dhandle_buckets=512),hazard_max=1000,"
    "heuristic_controls=(checkpoint_cleanup_obsolete_tw_pages_dirty_max=100"
    ",eviction_obsolete_tw_pages_dirty_max=100,"
    "obsolete_tw_btree_max=100),history_store=(file_max=0),"
    "io_capacity=(chunk_cache=0,total=0),json_output=[],"
    "live_restore=(enabled=false,path=,read_size=1MB,threads_max=8),"
    "log=(archive=true,compressor=,enabled=false,file_max=100MB,"
    "force_write_wait=0,os_cache_dirty_pct=0,path=\".\",prealloc=true"
    ",prealloc_init_count=1,recover=on,remove=true,zero_fill=false),"
    "mmap=true,mmap_all=false,multiprocess=false,"
    "operation_timeout_ms=0,operation_tracking=(enabled=false,"
    "path=\".\"),page_delta=(delta_pct=20,"
    "flatten_leaf_page_delta=false,internal_page_delta=true,"
    "leaf_page_delta=true,max_consecutive_delta=32),"
    "precise_checkpoint=false,prefetch=(available=false,"
    "default=false),preserve_prepared=false,readonly=false,"
    "rollback_to_stable=(threads=4),salvage=false,session_max=100,"
    "session_scratch_max=2MB,session_table_cache=true,"
    "shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
    "statistics=none,statistics_log=(json=false,on_close=false,"
    "path=\".\",sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
    "tiered_storage=(auth_token=,bucket=,bucket_prefix=,"
    "cache_directory=,interval=60,local_retention=300,name=,"
    "shared=false),timing_stress_for_test=,"
    "transaction_sync=(enabled=false,method=fsync),verbose=[],"
    "verify_metadata=false,version=(major=0,minor=0),write_through=",
    confchk_wiredtiger_open_basecfg, 67, confchk_wiredtiger_open_basecfg_jump, 54,
    WT_CONF_SIZING_NONE, false},
  {"wiredtiger_open_usercfg",
    "backup_restore_target=,"
    "block_cache=(blkcache_eviction_aggression=1800,"
    "cache_on_checkpoint=true,cache_on_writes=true,enabled=false,"
    "full_target=95,hashsize=32768,max_percent_overhead=10,"
    "nvram_path=,percent_file_in_dram=50,size=0,system_ram=0,type=),"
    "buffer_alignment=,builtin_extension_config=,cache_cursors=true,"
    "cache_eviction_controls=(incremental_app_eviction=false,"
    "scrub_evict_under_target_limit=true),cache_max_wait_ms=0,"
    "cache_overhead=8,cache_size=100MB,cache_stuck_timeout_ms=300000,"
    "checkpoint=(log_size=0,wait=0),"
    "checkpoint_cleanup=(file_wait_ms=0,method=none,wait=300),"
    "checkpoint_sync=true,chunk_cache=(capacity=10GB,"
    "chunk_cache_evict_trigger=90,chunk_size=1MB,enabled=false,"
    "flushed_data_cache_insertion=true,hashsize=1024,pinned=,"
    "storage_path=,type=FILE),compatibility=(release=,require_max=,"
    "require_min=),compile_configuration_count=1000,"
    "debug_mode=(background_compact=false,checkpoint_retention=0,"
    "configuration=false,corruption_abort=true,cursor_copy=false,"
    "cursor_reposition=false,eviction=false,"
    "eviction_checkpoint_ts_ordering=false,log_retention=0,"
    "page_history=false,realloc_exact=false,realloc_malloc=false,"
    "rollback_error=0,slow_checkpoint=false,stress_skiplist=false,"
    "table_logging=false,tiered_flush_error_continue=false,"
    "update_restore_evict=false),direct_io=,"
    "disaggregated=(checkpoint_meta=,last_materialized_lsn=,"
    "local_files_action=delete,lose_all_my_data=false,page_log=,"
    "role=),encryption=(keyid=,name=,secretkey=),error_prefix=,"
    "eviction=(evict_sample_inmem=true,evict_use_softptr=false,"
    "legacy_page_visit_strategy=false,threads_max=8,threads_min=1),"
    "eviction_checkpoint_target=1,eviction_dirty_target=5,"
    "eviction_dirty_trigger=20,eviction_target=80,eviction_trigger=95"
    ",eviction_updates_target=0,eviction_updates_trigger=0,"
    "extensions=,extra_diagnostics=[],file_extend=,"
    "file_manager=(close_handle_minimum=250,close_idle_time=30,"
    "close_scan_interval=10),generation_drain_timeout_ms=240000,"
    "hash=(buckets=512,dhandle_buckets=512),hazard_max=1000,"
    "heuristic_controls=(checkpoint_cleanup_obsolete_tw_pages_dirty_max=100"
    ",eviction_obsolete_tw_pages_dirty_max=100,"
    "obsolete_tw_btree_max=100),history_store=(file_max=0),"
    "io_capacity=(chunk_cache=0,total=0),json_output=[],"
    "live_restore=(enabled=false,path=,read_size=1MB,threads_max=8),"
    "log=(archive=true,compressor=,enabled=false,file_max=100MB,"
    "force_write_wait=0,os_cache_dirty_pct=0,path=\".\",prealloc=true"
    ",prealloc_init_count=1,recover=on,remove=true,zero_fill=false),"
    "mmap=true,mmap_all=false,multiprocess=false,"
    "operation_timeout_ms=0,operation_tracking=(enabled=false,"
    "path=\".\"),page_delta=(delta_pct=20,"
    "flatten_leaf_page_delta=false,internal_page_delta=true,"
    "leaf_page_delta=true,max_consecutive_delta=32),"
    "precise_checkpoint=false,prefetch=(available=false,"
    "default=false),preserve_prepared=false,readonly=false,"
    "rollback_to_stable=(threads=4),salvage=false,session_max=100,"
    "session_scratch_max=2MB,session_table_cache=true,"
    "shared_cache=(chunk=10MB,name=,quota=0,reserve=0,size=500MB),"
    "statistics=none,statistics_log=(json=false,on_close=false,"
    "path=\".\",sources=,timestamp=\"%b %d %H:%M:%S\",wait=0),"
    "tiered_storage=(auth_token=,bucket=,bucket_prefix=,"
    "cache_directory=,interval=60,local_retention=300,name=,"
    "shared=false),timing_stress_for_test=,"
    "transaction_sync=(enabled=false,method=fsync),verbose=[],"
    "verify_metadata=false,write_through=",
    confchk_wiredtiger_open_usercfg, 66, confchk_wiredtiger_open_usercfg_jump, 55,
    WT_CONF_SIZING_NONE, false},
  {NULL, NULL, NULL, 0, NULL, 0, WT_CONF_SIZING_NONE, false}};

int
__wt_conn_config_init(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    const WT_CONFIG_ENTRY *ep, **epp;

    conn = S2C(session);

    /* Build a list of pointers to the configuration information. */
    WT_RET(__wt_calloc_def(session, WT_ELEMENTS(config_entries), &epp));
    conn->config_entries = epp;

    /* Fill in the list to reference the default information. */
    for (ep = config_entries;;) {
        *epp++ = ep++;
        if (ep->method == NULL)
            break;
    }
    return (0);
}

void
__wt_conn_config_discard(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    __wt_free(session, conn->config_entries);
}

/*
 * __wt_conn_config_match --
 *     Return the static configuration entry for a method.
 */
const WT_CONFIG_ENTRY *
__wt_conn_config_match(const char *method)
{
    const WT_CONFIG_ENTRY *ep;

    for (ep = config_entries; ep->method != NULL; ++ep)
        if (strcmp(method, ep->method) == 0)
            return (ep);
    return (NULL);
}
