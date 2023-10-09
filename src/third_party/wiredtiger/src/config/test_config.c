/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_cache_hs_insert_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN, INT64_MAX,
    NULL},
  {"min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX, NULL},
  {"postrun", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"runtime", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"save", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_cache_hs_insert_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_cc_pages_removed_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN, INT64_MAX,
    NULL},
  {"min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX, NULL},
  {"postrun", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"runtime", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"save", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_cc_pages_removed_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_stat_cache_size_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN, INT64_MAX,
    NULL},
  {"min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX, NULL},
  {"postrun", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"runtime", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"save", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_stat_cache_size_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_stat_db_size_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN, INT64_MAX,
    NULL},
  {"min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX, NULL},
  {"postrun", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"runtime", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"save", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_stat_db_size_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_metrics_monitor_subconfigs[] = {
  {"cache_hs_insert", "category", NULL, NULL, confchk_cache_hs_insert_subconfigs, 5,
    confchk_cache_hs_insert_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"cc_pages_removed", "category", NULL, NULL, confchk_cc_pages_removed_subconfigs, 5,
    confchk_cc_pages_removed_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"stat_cache_size", "category", NULL, NULL, confchk_stat_cache_size_subconfigs, 5,
    confchk_stat_cache_size_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"stat_db_size", "category", NULL, NULL, confchk_stat_db_size_subconfigs, 5,
    confchk_stat_db_size_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_metrics_monitor_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};

static const WT_CONFIG_CHECK confchk_operation_tracker_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"tracking_key_format", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    INT64_MIN, INT64_MAX, NULL},
  {"tracking_value_format", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    INT64_MIN, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_operation_tracker_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

static const WT_CONFIG_CHECK confchk_statistics_config_subconfigs[] = {
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"type", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_statistics_config_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_timestamp_manager_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"oldest_lag", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    1000000, NULL},
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"stable_lag", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    1000000, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_timestamp_manager_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

static const WT_CONFIG_CHECK confchk_background_compact_config_subconfigs[] = {
  {"free_space_target_mb", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    INT64_MIN, INT64_MAX, NULL},
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"thread_count", "int", NULL, "min=0,max=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, 1,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_background_compact_config_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] =
  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

static const WT_CONFIG_CHECK confchk_checkpoint_config_subconfigs[] = {
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"thread_count", "int", NULL, "min=0,max=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, 1,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_checkpoint_config_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_ops_per_transaction_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN, INT64_MAX,
    NULL},
  {"min", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_ops_per_transaction_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

static const WT_CONFIG_CHECK confchk_custom_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2,
    confchk_ops_per_transaction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"thread_count", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX,
    NULL},
  {"value_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_custom_config_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 1, 1, 1, 1, 3, 3, 3, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_insert_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2,
    confchk_ops_per_transaction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"thread_count", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX,
    NULL},
  {"value_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_insert_config_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 1, 1, 1, 1, 3, 3, 3, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_populate_config_subconfigs[] = {
  {"collection_count", "int", NULL, "min=0,max=200000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 200000, NULL},
  {"key_count_per_collection", "int", NULL, "min=0,max=1000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 1000000, NULL},
  {"key_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {"thread_count", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"value_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_populate_config_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
  1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_read_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2,
    confchk_ops_per_transaction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"thread_count", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX,
    NULL},
  {"value_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_read_config_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 1, 1, 1, 1, 3, 3, 3, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_remove_config_subconfigs[] = {
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2,
    confchk_ops_per_transaction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"thread_count", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_remove_config_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

static const WT_CONFIG_CHECK confchk_update_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2,
    confchk_ops_per_transaction_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"thread_count", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0, INT64_MAX,
    NULL},
  {"value_size", "int", NULL, "min=1", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 1, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_update_config_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 1, 1, 1, 1, 3, 3, 3, 3, 3, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5};

static const WT_CONFIG_CHECK confchk_workload_manager_subconfigs[] = {
  {"background_compact_config", "category", NULL, NULL,
    confchk_background_compact_config_subconfigs, 3,
    confchk_background_compact_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"checkpoint_config", "category", NULL, NULL, confchk_checkpoint_config_subconfigs, 2,
    confchk_checkpoint_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"custom_config", "category", NULL, NULL, confchk_custom_config_subconfigs, 5,
    confchk_custom_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"insert_config", "category", NULL, NULL, confchk_insert_config_subconfigs, 5,
    confchk_insert_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"op_rate", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"populate_config", "category", NULL, NULL, confchk_populate_config_subconfigs, 5,
    confchk_populate_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"read_config", "category", NULL, NULL, confchk_read_config_subconfigs, 5,
    confchk_read_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"remove_config", "category", NULL, NULL, confchk_remove_config_subconfigs, 3,
    confchk_remove_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"update_config", "category", NULL, NULL, confchk_update_config_subconfigs, 5,
    confchk_update_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_workload_manager_subconfigs_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 3,
  4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 6, 7, 7, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};

static const WT_CONFIG_CHECK confchk_background_compact[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_background_compact_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6, 6,
  6, 6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_bounded_cursor_perf[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_bounded_cursor_perf_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6,
  6, 6, 6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_bounded_cursor_prefix_indices[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_bounded_cursor_prefix_indices_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4,
  5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_bounded_cursor_prefix_search_near[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_bounded_cursor_prefix_search_near_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
  4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_bounded_cursor_prefix_stat[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"search_near_threads", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_bounded_cursor_prefix_stat_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5,
  6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 11, 12, 12, 13, 14, 14, 14, 14, 14, 14, 14, 14};

static const WT_CONFIG_CHECK confchk_bounded_cursor_stress[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_bounded_cursor_stress_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6,
  6, 6, 6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_burst_inserts[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"burst_duration", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING, INT64_MIN,
    INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_burst_inserts_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 5, 6, 7, 7, 7, 7,
  7, 7, 7, 7, 8, 8, 9, 9, 9, 10, 11, 12, 12, 13, 14, 14, 14, 14, 14, 14, 14, 14};

static const WT_CONFIG_CHECK confchk_cache_resize[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_cache_resize_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6, 6, 6, 6,
  6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_hs_cleanup[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_hs_cleanup_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6, 6, 6, 6,
  6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_operations_test[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_operations_test_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6, 6, 6,
  6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_reverse_split[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_reverse_split_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6, 6, 6,
  6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_search_near_01[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"search_near_threads", "string", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_STRING,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_search_near_01_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6, 6, 6,
  6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 11, 12, 12, 13, 14, 14, 14, 14, 14, 14, 14, 14};

static const WT_CONFIG_CHECK confchk_search_near_02[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_search_near_02_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6, 6, 6,
  6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_search_near_03[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_search_near_03_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6, 6, 6,
  6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_CHECK confchk_test_template[] = {
  {"background_compact_debug_mode", "boolean", NULL, NULL, NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN, INT64_MAX, NULL},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT, 0,
    INT64_MAX, NULL},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0, NULL,
    WT_CONFIG_COMPILED_TYPE_INT, 0, 100000000000, NULL},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_INT,
    0, 1000000, NULL},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6,
    confchk_metrics_monitor_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN, INT64_MAX,
    NULL},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4,
    confchk_operation_tracker_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN,
    INT64_MIN, INT64_MAX, NULL},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2,
    confchk_statistics_config_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4,
    confchk_timestamp_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {"validate", "boolean", NULL, NULL, NULL, 0, NULL, WT_CONFIG_COMPILED_TYPE_BOOLEAN, INT64_MIN,
    INT64_MAX, NULL},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 10,
    confchk_workload_manager_subconfigs_jump, WT_CONFIG_COMPILED_TYPE_CATEGORY, INT64_MIN,
    INT64_MAX, NULL},
  {NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL}};

static const uint8_t confchk_test_template_jump[WT_CONFIG_JUMP_TABLE_SIZE] = {0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 5, 6, 6, 6, 6,
  6, 6, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 11, 12, 13, 13, 13, 13, 13, 13, 13, 13};

static const WT_CONFIG_ENTRY config_entries[] = {
  {"background_compact",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_background_compact, 13, confchk_background_compact_jump},
  {"bounded_cursor_perf",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_bounded_cursor_perf, 13, confchk_bounded_cursor_perf_jump},
  {"bounded_cursor_prefix_indices",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_bounded_cursor_prefix_indices, 13, confchk_bounded_cursor_prefix_indices_jump},
  {"bounded_cursor_prefix_search_near",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_bounded_cursor_prefix_search_near, 13, confchk_bounded_cursor_prefix_search_near_jump},
  {"bounded_cursor_prefix_stat",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,search_near_threads=10,"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_bounded_cursor_prefix_stat, 14, confchk_bounded_cursor_prefix_stat_jump},
  {"bounded_cursor_stress",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_bounded_cursor_stress, 13, confchk_bounded_cursor_stress_jump},
  {"burst_inserts",
    "background_compact_debug_mode=false,burst_duration=90,"
    "cache_max_wait_ms=0,cache_size_mb=0,compression_enabled=false,"
    "duration_seconds=0,enable_logging=false,"
    "metrics_monitor=(cache_hs_insert=(max=1,min=0,postrun=false,"
    "runtime=false,save=false),cc_pages_removed=(max=1,min=0,"
    "postrun=false,runtime=false,save=false),enabled=true,op_rate=1s,"
    "stat_cache_size=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),stat_db_size=(max=1,min=0,postrun=false,"
    "runtime=false,save=false)),operation_tracker=(enabled=true,"
    "op_rate=1s,tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_burst_inserts, 14, confchk_burst_inserts_jump},
  {"cache_resize",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_cache_resize, 13, confchk_cache_resize_jump},
  {"hs_cleanup",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_hs_cleanup, 13, confchk_hs_cleanup_jump},
  {"operations_test",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_operations_test, 13, confchk_operations_test_jump},
  {"reverse_split",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_reverse_split, 13, confchk_reverse_split_jump},
  {"search_near_01",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,search_near_threads=10,"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_search_near_01, 14, confchk_search_near_01_jump},
  {"search_near_02",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_search_near_02, 13, confchk_search_near_02_jump},
  {"search_near_03",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_search_near_03, 13, confchk_search_near_03_jump},
  {"test_template",
    "background_compact_debug_mode=false,cache_max_wait_ms=0,"
    "cache_size_mb=0,compression_enabled=false,duration_seconds=0,"
    "enable_logging=false,metrics_monitor=(cache_hs_insert=(max=1,"
    "min=0,postrun=false,runtime=false,save=false),"
    "cc_pages_removed=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),enabled=true,op_rate=1s,stat_cache_size=(max=1,min=0"
    ",postrun=false,runtime=false,save=false),stat_db_size=(max=1,"
    "min=0,postrun=false,runtime=false,save=false)),"
    "operation_tracker=(enabled=true,op_rate=1s,"
    "tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,statistics_config=(enable_logging=true,"
    "type=all),timestamp_manager=(enabled=true,oldest_lag=1,"
    "op_rate=1s,stable_lag=1),validate=true,"
    "workload_manager=(background_compact_config=(free_space_target_mb=20"
    ",op_rate=1s,thread_count=0),checkpoint_config=(op_rate=60s,"
    "thread_count=1),custom_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "enabled=true,insert_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "op_rate=1s,populate_config=(collection_count=1,"
    "key_count_per_collection=0,key_size=5,thread_count=1,"
    "value_size=5),read_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5),"
    "remove_config=(op_rate=1s,ops_per_transaction=(max=1,min=0),"
    "thread_count=0),update_config=(key_size=5,op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0,value_size=5))",
    confchk_test_template, 13, confchk_test_template_jump},
  {NULL, NULL, NULL, 0, NULL}};

/*
 * __wt_test_config_match --
 *     Return the static configuration entry for a test.
 */
const WT_CONFIG_ENTRY *
__wt_test_config_match(const char *test_name)
{
    const WT_CONFIG_ENTRY *ep;

    for (ep = config_entries; ep->method != NULL; ++ep)
        if (strcmp(test_name, ep->method) == 0)
            return (ep);
    return (NULL);
}
