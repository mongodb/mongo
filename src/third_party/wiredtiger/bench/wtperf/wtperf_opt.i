/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * wtperf_opt.i
 *	List of options for wtperf.  This is included multiple times.
 */

#ifdef OPT_DECLARE_STRUCT
#define	DEF_OPT_AS_BOOL(name, initval, desc)		int name;
#define	DEF_OPT_AS_CONFIG_STRING(name, initval, desc)	const char *name;
#define	DEF_OPT_AS_STRING(name, initval, desc)		const char *name;
#define	DEF_OPT_AS_UINT32(name, initval, desc)		uint32_t name;
#endif

#ifdef OPT_DEFINE_DESC
#define	DEF_OPT_AS_BOOL(name, initval, desc)				\
	{ #name, desc, #initval, BOOL_TYPE, offsetof(CONFIG, name) },
#define	DEF_OPT_AS_CONFIG_STRING(name, initval, desc)			\
	{ #name, desc, initval, CONFIG_STRING_TYPE,                    \
	offsetof(CONFIG, name) },
#define	DEF_OPT_AS_STRING(name, initval, desc)				\
	{ #name, desc, initval, STRING_TYPE, offsetof(CONFIG, name) },
#define	DEF_OPT_AS_UINT32(name, initval, desc)				\
	{ #name, desc, #initval, UINT32_TYPE, offsetof(CONFIG, name) },
#endif

#ifdef OPT_DEFINE_DEFAULT
#define	DEF_OPT_AS_BOOL(name, initval, desc)		initval,
#define	DEF_OPT_AS_CONFIG_STRING(name, initval, desc)	initval,
#define	DEF_OPT_AS_STRING(name, initval, desc)		initval,
#define	DEF_OPT_AS_UINT32(name, initval, desc)		initval,
#endif

#ifdef OPT_DEFINE_DOXYGEN
#define	DEF_OPT_AS_BOOL(name, initval, desc)				\
	{ #name, desc, #initval, BOOL_TYPE, 0 },
#define	DEF_OPT_AS_CONFIG_STRING(name, initval, desc)			\
	{ #name, desc, initval, CONFIG_STRING_TYPE, 0 },
#define	DEF_OPT_AS_STRING(name, initval, desc)				\
	{ #name, desc, initval, STRING_TYPE, 0 },
#define	DEF_OPT_AS_UINT32(name, initval, desc)				\
	{ #name, desc, #initval, UINT32_TYPE, 0 },
#endif

/*
 * Each option listed here represents a CONFIG struct field that may be
 * altered on command line via -o and -O.  Each option appears here as:
 *    DEF_OPT_AS_BOOL(name, initval, desc)
 *    DEF_OPT_AS_CONFIG_STRING(name, initval, desc)
 *    DEF_OPT_AS_STRING(name, initval, desc)
 *    DEF_OPT_AS_UINT32(name, initval, desc)
 *
 * The first four forms (*_{CONFIG_STRING|STRING|BOOL|UINT}) have these
 * parameters:
 *    name:     a C identifier, this identifier will be a field in CONFIG,
 *              and identifies the option for -o or -O.
 *    initval:  a default initial value for the field.
 *              The default values are tiny, we want the basic run to be fast.
 *    desc:     a description that will appear in the usage message.
 *
 * The difference between CONFIG_STRING and STRING is that CONFIG_STRING
 * options are appended to existing content, whereas STRING options overwrite.
 */
DEF_OPT_AS_UINT32(async_threads, 0, "number of async worker threads")
DEF_OPT_AS_UINT32(checkpoint_interval, 120,
    "checkpoint every interval seconds during the workload phase.")
DEF_OPT_AS_UINT32(checkpoint_stress_rate, 0,
    "checkpoint every rate operations during the populate phase in the "
    "populate thread(s), 0 to disable")
DEF_OPT_AS_UINT32(checkpoint_threads, 0, "number of checkpoint threads")
DEF_OPT_AS_CONFIG_STRING(conn_config, "create",
    "connection configuration string")
DEF_OPT_AS_BOOL(compact, 0, "post-populate compact for LSM merging activity")
DEF_OPT_AS_STRING(compression, "none",
    "compression extension.  Allowed configuration values are: "
    "'none', 'lz4', 'snappy', 'zlib'")
DEF_OPT_AS_BOOL(create, 1,
    "do population phase; false to use existing database")
DEF_OPT_AS_UINT32(database_count, 1,
    "number of WiredTiger databases to use. Each database will execute the"
    " workload using a separate home directory and complete set of worker"
    " threads")
DEF_OPT_AS_UINT32(drop_tables, 0,
    "Whether to drop all tables at the end of the run, and report time taken"
    " to do the drop.")
DEF_OPT_AS_UINT32(icount, 5000,
    "number of records to initially populate. If multiple tables are "
    "configured the count is spread evenly across all tables.")
DEF_OPT_AS_UINT32(idle_table_cycle, 0,
    "Enable regular create and drop of idle tables, value is the maximum "
    "number of seconds a create or drop is allowed before flagging an error. "
    "Default 0 which means disabled.")
DEF_OPT_AS_BOOL(index, 0,
    "Whether to create an index on the value field.")
DEF_OPT_AS_BOOL(insert_rmw, 0,
    "execute a read prior to each insert in workload phase")
DEF_OPT_AS_UINT32(key_sz, 20, "key size")
DEF_OPT_AS_BOOL(log_partial, 0, "perform partial logging on first table only.")
DEF_OPT_AS_UINT32(min_throughput, 0,
    "notify if any throughput measured is less than this amount. "
    "Aborts or prints warning based on min_throughput_fatal setting. "
    "Requires sample_interval to be configured")
DEF_OPT_AS_BOOL(min_throughput_fatal, 0,
    "print warning (false) or abort (true) of min_throughput failure.")
DEF_OPT_AS_UINT32(max_latency, 0,
    "notify if any latency measured exceeds this number of milliseconds."
    "Aborts or prints warning based on min_throughput_fatal setting. "
    "Requires sample_interval to be configured")
DEF_OPT_AS_BOOL(max_latency_fatal, 0,
    "print warning (false) or abort (true) of max_latency failure.")
DEF_OPT_AS_UINT32(pareto, 0, "use pareto distribution for random numbers. Zero "
    "to disable, otherwise a percentage indicating how aggressive the "
    "distribution should be.")
DEF_OPT_AS_UINT32(populate_ops_per_txn, 0,
    "number of operations to group into each transaction in the populate "
    "phase, zero for auto-commit")
DEF_OPT_AS_UINT32(populate_threads, 1,
    "number of populate threads, 1 for bulk load")
DEF_OPT_AS_UINT32(random_range, 0,
    "if non zero choose a value from within this range as the key for "
    "insert operations")
DEF_OPT_AS_BOOL(random_value, 0, "generate random content for the value")
DEF_OPT_AS_UINT32(read_range, 0, "scan a range of keys after each search")
DEF_OPT_AS_BOOL(readonly, 0,
    "reopen the connection between populate and workload phases in readonly "
    "mode.  Requires reopen_connection turned on (default).  Requires that "
    "read be the only workload specified")
DEF_OPT_AS_BOOL(reopen_connection, 1,
    "close and reopen the connection between populate and workload phases")
DEF_OPT_AS_UINT32(report_interval, 2,
    "output throughput information every interval seconds, 0 to disable")
DEF_OPT_AS_UINT32(run_ops, 0,
    "total read, insert and update workload operations")
DEF_OPT_AS_UINT32(run_time, 0,
    "total workload seconds")
DEF_OPT_AS_UINT32(sample_interval, 0,
    "performance logging every interval seconds, 0 to disable")
DEF_OPT_AS_UINT32(sample_rate, 50,
    "how often the latency of operations is measured. One for every operation,"
    "two for every second operation, three for every third operation etc.")
DEF_OPT_AS_CONFIG_STRING(sess_config, "", "session configuration string")
DEF_OPT_AS_UINT32(session_count_idle, 0,
    "number of idle sessions to create. Default 0.")
DEF_OPT_AS_CONFIG_STRING(table_config,
    "key_format=S,value_format=S,type=lsm,exclusive=true,"
    "allocation_size=4kb,internal_page_max=64kb,leaf_page_max=4kb,"
    "split_pct=100",
    "table configuration string")
DEF_OPT_AS_UINT32(table_count, 1,
    "number of tables to run operations over. Keys are divided evenly "
    "over the tables. Cursors are held open on all tables. Default 1, maximum "
    "99999.")
DEF_OPT_AS_UINT32(table_count_idle, 0,
    "number of tables to create, that won't be populated. Default 0.")
DEF_OPT_AS_STRING(threads, "", "workload configuration: each 'count' "
    "entry is the total number of threads, and the 'insert', 'read' and "
    "'update' entries are the ratios of insert, read and update operations "
    "done by each worker thread; If a throttle value is provided each thread "
    "will do a maximum of that number of operations per second; multiple "
    "workload configurations may be specified per threads configuration; "
    "for example, a more complex threads configuration might be "
    "'threads=((count=2,reads=1)(count=8,reads=1,inserts=2,updates=1))' "
    "which would create 2 threads doing nothing but reads and 8 threads "
    "each doing 50% inserts and 25% reads and updates.  Allowed configuration "
    "values are 'count', 'throttle', 'update_delta', 'reads', 'inserts', "
    "'updates', 'truncate', 'truncate_pct' and 'truncate_count'. There are "
    "also behavior modifiers, supported modifiers are 'ops_per_txn'")
DEF_OPT_AS_CONFIG_STRING(transaction_config, "",
    "transaction configuration string, relevant when populate_opts_per_txn "
    "is nonzero")
DEF_OPT_AS_STRING(table_name, "test", "table name")
DEF_OPT_AS_UINT32(value_sz_max, 1000,
    "maximum value size when delta updates are present. Default disabled")
DEF_OPT_AS_UINT32(value_sz_min, 1,
    "minimum value size when delta updates are present. Default disabled")
DEF_OPT_AS_UINT32(value_sz, 100, "value size")
DEF_OPT_AS_UINT32(verbose, 1, "verbosity")
DEF_OPT_AS_UINT32(warmup, 0,
    "How long to run the workload phase before starting measurements")

#undef DEF_OPT_AS_BOOL
#undef DEF_OPT_AS_CONFIG_STRING
#undef DEF_OPT_AS_STRING
#undef DEF_OPT_AS_UINT32
