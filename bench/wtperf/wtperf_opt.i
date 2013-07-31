/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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
#define DEF_OPT_AS_STRING(name, initval)	const char *name;
#define DEF_OPT_AS_BOOL(name, initval)		uint32_t name;
#define DEF_OPT_AS_UINT32(name, initval)	uint32_t name;
#define DEF_OPT_AS_FLAGVAL(name, bits)
#endif

#ifdef OPT_DEFINE_DESC
#define DEF_OPT_AS_STRING(name, initval)			\
	{ #name, STRING_TYPE, offsetof(CONFIG, name), 0 },
#define DEF_OPT_AS_BOOL(name, initval)				\
	{ #name, BOOL_TYPE, offsetof(CONFIG, name), 0 },
#define DEF_OPT_AS_UINT32(name, initval)			\
	{ #name, UINT32_TYPE, offsetof(CONFIG, name), 0 },
#define DEF_OPT_AS_FLAGVAL(name, bits)				\
	{ #name, FLAG_TYPE, offsetof(CONFIG, flags), bits },
#endif

#ifdef OPT_DEFINE_DEFAULT
#define DEF_OPT_AS_STRING(name, initval)	initval,
#define DEF_OPT_AS_BOOL(name, initval)		initval,
#define DEF_OPT_AS_UINT32(name, initval)	initval,
#define DEF_OPT_AS_FLAGVAL(name, bits)
#endif

/*
 * CONFIG struct fields that may be altered on command line via -o and -O.
 * The default values are tiny, we want the basic run to be fast.
 */
DEF_OPT_AS_UINT32(checkpoint_interval, 0)	/* Zero to disable. */
DEF_OPT_AS_STRING(conn_config, "create,cache_size=200MB")
DEF_OPT_AS_BOOL(create, 1)		/* Whether to populate for this run. */
DEF_OPT_AS_UINT32(data_sz, 100)
DEF_OPT_AS_UINT32(icount, 5000)		/* Items to insert. */
DEF_OPT_AS_UINT32(insert_threads, 0)	/* Number of insert threads. */
DEF_OPT_AS_UINT32(key_sz, 20)
DEF_OPT_AS_UINT32(populate_threads, 1)	/* Number of populate threads. */
DEF_OPT_AS_UINT32(rand_seed, 14023954)
DEF_OPT_AS_UINT32(random_range, 0)
DEF_OPT_AS_UINT32(read_threads, 2)	/* Number of read threads. */
DEF_OPT_AS_UINT32(report_interval, 2)
DEF_OPT_AS_UINT32(run_time, 2)
DEF_OPT_AS_UINT32(stat_interval, 0)	/* Zero to disable. */
DEF_OPT_AS_STRING(table_config, DEFAULT_LSM_CONFIG)
DEF_OPT_AS_UINT32(update_threads, 0)	/* Number of update threads. */
DEF_OPT_AS_STRING(uri, "lsm:test")
DEF_OPT_AS_UINT32(verbose, 0)

/* values for CONFIG.flags */
DEF_OPT_AS_FLAGVAL(insert_rmw, PERF_INSERT_RMW)
DEF_OPT_AS_FLAGVAL(pareto, PERF_RAND_PARETO)
DEF_OPT_AS_FLAGVAL(random, PERF_RAND_WORKLOAD)

#undef DEF_OPT_AS_STRING
#undef DEF_OPT_AS_BOOL
#undef DEF_OPT_AS_UINT32
#undef DEF_OPT_AS_FLAGVAL
