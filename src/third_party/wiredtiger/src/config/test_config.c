/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"

static const WT_CONFIG_CHECK confchk_cache_hs_insert_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0}, {"min", "int", NULL, "min=0", NULL, 0},
  {"postrun", "boolean", NULL, NULL, NULL, 0}, {"runtime", "boolean", NULL, NULL, NULL, 0},
  {"save", "boolean", NULL, NULL, NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_cc_pages_removed_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0}, {"min", "int", NULL, "min=0", NULL, 0},
  {"postrun", "boolean", NULL, NULL, NULL, 0}, {"runtime", "boolean", NULL, NULL, NULL, 0},
  {"save", "boolean", NULL, NULL, NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_stat_cache_size_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0}, {"min", "int", NULL, "min=0", NULL, 0},
  {"postrun", "boolean", NULL, NULL, NULL, 0}, {"runtime", "boolean", NULL, NULL, NULL, 0},
  {"save", "boolean", NULL, NULL, NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_stat_db_size_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0}, {"min", "int", NULL, "min=0", NULL, 0},
  {"postrun", "boolean", NULL, NULL, NULL, 0}, {"runtime", "boolean", NULL, NULL, NULL, 0},
  {"save", "boolean", NULL, NULL, NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_metrics_monitor_subconfigs[] = {
  {"cache_hs_insert", "category", NULL, NULL, confchk_cache_hs_insert_subconfigs, 5},
  {"cc_pages_removed", "category", NULL, NULL, confchk_cc_pages_removed_subconfigs, 5},
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"stat_cache_size", "category", NULL, NULL, confchk_stat_cache_size_subconfigs, 5},
  {"stat_db_size", "category", NULL, NULL, confchk_stat_db_size_subconfigs, 5},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_operation_tracker_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"tracking_key_format", "string", NULL, NULL, NULL, 0},
  {"tracking_value_format", "string", NULL, NULL, NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_statistics_config_subconfigs[] = {
  {"enable_logging", "boolean", NULL, NULL, NULL, 0}, {"type", "string", NULL, NULL, NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_timestamp_manager_subconfigs[] = {
  {"enabled", "boolean", NULL, NULL, NULL, 0},
  {"oldest_lag", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"op_rate", "string", NULL, NULL, NULL, 0},
  {"stable_lag", "int", NULL, "min=0,max=1000000", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_checkpoint_config_subconfigs[] = {
  {"op_rate", "string", NULL, NULL, NULL, 0}, {"thread_count", "int", NULL, "min=0,max=1", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_ops_per_transaction_subconfigs[] = {
  {"max", "string", NULL, NULL, NULL, 0}, {"min", "int", NULL, "min=0", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_custom_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=1", NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "int", NULL, "min=0", NULL, 0}, {"value_size", "int", NULL, "min=1", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_insert_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=1", NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "int", NULL, "min=0", NULL, 0}, {"value_size", "int", NULL, "min=1", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_populate_config_subconfigs[] = {
  {"collection_count", "int", NULL, "min=0,max=200000", NULL, 0},
  {"key_count_per_collection", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"key_size", "int", NULL, "min=1", NULL, 0}, {"thread_count", "string", NULL, NULL, NULL, 0},
  {"value_size", "int", NULL, "min=1", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_read_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=1", NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "int", NULL, "min=0", NULL, 0}, {"value_size", "int", NULL, "min=1", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_remove_config_subconfigs[] = {
  {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "int", NULL, "min=0", NULL, 0}, {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_update_config_subconfigs[] = {
  {"key_size", "int", NULL, "min=1", NULL, 0}, {"op_rate", "string", NULL, NULL, NULL, 0},
  {"ops_per_transaction", "category", NULL, NULL, confchk_ops_per_transaction_subconfigs, 2},
  {"thread_count", "int", NULL, "min=0", NULL, 0}, {"value_size", "int", NULL, "min=1", NULL, 0},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_workload_manager_subconfigs[] = {
  {"checkpoint_config", "category", NULL, NULL, confchk_checkpoint_config_subconfigs, 2},
  {"custom_config", "category", NULL, NULL, confchk_custom_config_subconfigs, 5},
  {"enabled", "boolean", NULL, NULL, NULL, 0},
  {"insert_config", "category", NULL, NULL, confchk_insert_config_subconfigs, 5},
  {"op_rate", "string", NULL, NULL, NULL, 0},
  {"populate_config", "category", NULL, NULL, confchk_populate_config_subconfigs, 5},
  {"read_config", "category", NULL, NULL, confchk_read_config_subconfigs, 5},
  {"remove_config", "category", NULL, NULL, confchk_remove_config_subconfigs, 3},
  {"update_config", "category", NULL, NULL, confchk_update_config_subconfigs, 5},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_bounded_cursor_perf[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_bounded_cursor_prefix_indices[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_bounded_cursor_prefix_search_near[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_bounded_cursor_prefix_stat[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"search_near_threads", "string", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_bounded_cursor_stress[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_burst_inserts[] = {
  {"burst_duration", "string", NULL, NULL, NULL, 0},
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_cache_resize[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_hs_cleanup[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_operations_test[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_reverse_split[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_search_near_01[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"search_near_threads", "string", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_search_near_02[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_search_near_03[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_CHECK confchk_test_template[] = {
  {"cache_max_wait_ms", "int", NULL, "min=0", NULL, 0},
  {"cache_size_mb", "int", NULL, "min=0,max=100000000000", NULL, 0},
  {"compression_enabled", "boolean", NULL, NULL, NULL, 0},
  {"duration_seconds", "int", NULL, "min=0,max=1000000", NULL, 0},
  {"enable_logging", "boolean", NULL, NULL, NULL, 0},
  {"metrics_monitor", "category", NULL, NULL, confchk_metrics_monitor_subconfigs, 6},
  {"operation_tracker", "category", NULL, NULL, confchk_operation_tracker_subconfigs, 4},
  {"reverse_collator", "boolean", NULL, NULL, NULL, 0},
  {"statistics_config", "category", NULL, NULL, confchk_statistics_config_subconfigs, 2},
  {"timestamp_manager", "category", NULL, NULL, confchk_timestamp_manager_subconfigs, 4},
  {"workload_manager", "category", NULL, NULL, confchk_workload_manager_subconfigs, 9},
  {NULL, NULL, NULL, NULL, NULL, 0}};

static const WT_CONFIG_ENTRY config_entries[] = {
  {"bounded_cursor_perf",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_bounded_cursor_perf, 11},
  {"bounded_cursor_prefix_indices",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_bounded_cursor_prefix_indices, 11},
  {"bounded_cursor_prefix_search_near",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_bounded_cursor_prefix_search_near, 11},
  {"bounded_cursor_prefix_stat",
    "cache_max_wait_ms=0,cache_size_mb=0,compression_enabled=false,"
    "duration_seconds=0,enable_logging=false,"
    "metrics_monitor=(cache_hs_insert=(max=1,min=0,postrun=false,"
    "runtime=false,save=false),cc_pages_removed=(max=1,min=0,"
    "postrun=false,runtime=false,save=false),enabled=true,op_rate=1s,"
    "stat_cache_size=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),stat_db_size=(max=1,min=0,postrun=false,"
    "runtime=false,save=false)),operation_tracker=(enabled=true,"
    "op_rate=1s,tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,search_near_threads=10,"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_manager=(checkpoint_config=(op_rate=60s,"
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
    confchk_bounded_cursor_prefix_stat, 12},
  {"bounded_cursor_stress",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_bounded_cursor_stress, 11},
  {"burst_inserts",
    "burst_duration=90,cache_max_wait_ms=0,cache_size_mb=0,"
    "compression_enabled=false,duration_seconds=0,"
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_burst_inserts, 12},
  {"cache_resize",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_cache_resize, 11},
  {"hs_cleanup",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_hs_cleanup, 11},
  {"operations_test",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_operations_test, 11},
  {"reverse_split",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_reverse_split, 11},
  {"search_near_01",
    "cache_max_wait_ms=0,cache_size_mb=0,compression_enabled=false,"
    "duration_seconds=0,enable_logging=false,"
    "metrics_monitor=(cache_hs_insert=(max=1,min=0,postrun=false,"
    "runtime=false,save=false),cc_pages_removed=(max=1,min=0,"
    "postrun=false,runtime=false,save=false),enabled=true,op_rate=1s,"
    "stat_cache_size=(max=1,min=0,postrun=false,runtime=false,"
    "save=false),stat_db_size=(max=1,min=0,postrun=false,"
    "runtime=false,save=false)),operation_tracker=(enabled=true,"
    "op_rate=1s,tracking_key_format=QSQ,tracking_value_format=iS),"
    "reverse_collator=false,search_near_threads=10,"
    "statistics_config=(enable_logging=true,type=all),"
    "timestamp_manager=(enabled=true,oldest_lag=1,op_rate=1s,"
    "stable_lag=1),workload_manager=(checkpoint_config=(op_rate=60s,"
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
    confchk_search_near_01, 12},
  {"search_near_02",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_search_near_02, 11},
  {"search_near_03",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_search_near_03, 11},
  {"test_template",
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
    "op_rate=1s,stable_lag=1),"
    "workload_manager=(checkpoint_config=(op_rate=60s,thread_count=1)"
    ",custom_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1"
    ",min=0),thread_count=0,value_size=5),enabled=true,"
    "insert_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5),op_rate=1s,"
    "populate_config=(collection_count=1,key_count_per_collection=0,"
    "key_size=5,thread_count=1,value_size=5),read_config=(key_size=5,"
    "op_rate=1s,ops_per_transaction=(max=1,min=0),thread_count=0,"
    "value_size=5),remove_config=(op_rate=1s,"
    "ops_per_transaction=(max=1,min=0),thread_count=0),"
    "update_config=(key_size=5,op_rate=1s,ops_per_transaction=(max=1,"
    "min=0),thread_count=0,value_size=5))",
    confchk_test_template, 11},
  {NULL, NULL, NULL, 0}};

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
