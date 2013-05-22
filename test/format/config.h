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
 */

/*
 * Configuration for the wts program is an array of string-based parameters.
 * This is the structure used to declare them.
 */
typedef struct {
	const char	*name;			/* Configuration item */
	const char	*desc;			/* Configuration description */

#define	C_FIX		0x01			/* File types */
#define	C_VAR		0x02
#define	C_ROW		0x04
	uint8_t	 	type_mask;		/* File type mask */

	/* Value is a boolean, yes if roll of 1-to-100 is <= CONFIG->min. */
#define	C_BOOL		0x001

	/* Not a simple randomization, handle outside the main loop. */ 
#define	C_IGNORE	0x002

	/* Operation, only set if doing operations. */
#define	C_OPS		0x004

	/* Value was set from command-line or file, ignore for all runs. */
#define	C_PERM		0x008

	/* Value isn't random for this run, ignore just for this run. */
#define	C_TEMP		0x010

	/* Value is a string. */
#define	C_STRING	0x020
	uint32_t 	flags;

	uint32_t	min;			/* Minimum value */
	uint32_t	max;			/* Maximum value */
	u_int		*v;			/* Value for this run */
	char		**vstr;			/* Value for string options */
} CONFIG;

/*
 * Get a random value between a config min/max pair (inclusive for both min
 * and max).
 */
#define	CONF_RAND(cp)	MMRAND((cp)->min, (cp)->max)

static CONFIG c[] = {
	{ "bitcnt",
	  "number of bits for fixed-length column-store files",
	  C_FIX, 0, 1, 8, &g.c_bitcnt, NULL },

	{ "cache",
	  "size of the cache in MB",
	  0, 0, 1, 100, &g.c_cache, NULL },

	{ "compression",
	  "type of compression (none | bzip | lzo | raw | snappy)",
	  0, C_IGNORE|C_STRING, 1, 5, NULL, &g.c_compression },

	{ "data_extend",
	  "if data files are extended",			/* 5% */
	  0, C_BOOL, 5, 0, &g.c_data_extend, NULL },

	{ "data_source",
	  "data source (file | kvsbdb | lsm | memrata | table)",
	  0, C_IGNORE | C_STRING, 0, 0, NULL, &g.c_data_source },

	{ "delete_pct",
	  "percent operations that are deletes",
	  0, C_OPS, 0, 45, &g.c_delete_pct, NULL },

	{ "dictionary",
	  "if values are dictionary compressed",	/* 20% */
	  C_ROW | C_VAR, C_BOOL, 20, 0, &g.c_dictionary, NULL },

	{ "file_type",
	  "type of store to create (fix | var | row)",
	  0, C_IGNORE|C_STRING, 1, 3, NULL, &g.c_file_type },

	{ "huffman_key",
	  "if keys are huffman encoded",		/* 20% */
	  C_ROW, C_BOOL, 20, 0, &g.c_huffman_key, NULL },

	{ "huffman_value",
	 "if values are huffman encoded",		/* 20% */
	 C_ROW|C_VAR, C_BOOL, 20, 0, &g.c_huffman_value, NULL },

	{ "insert_pct",
	  "percent operations that are inserts",
	  0, C_OPS, 0, 45, &g.c_insert_pct, NULL },

	{ "internal_key_truncation",
	 "if values are huffman encoded",		/* 2% */
	 0, C_BOOL, 2, 0, &g.c_internal_key_truncation, NULL },

	{ "internal_page_max",
	  "maximum size of Btree internal nodes",
	  0, 0, 9, 17, &g.c_intl_page_max, NULL },

	{ "key_gap",
	  "gap between instantiated keys on a Btree page",
	  0, 0, 0, 20, &g.c_key_gap, NULL },

	{ "key_max",
	  "maximum size of keys",
	  C_ROW, 0, 64, 128, &g.c_key_max, NULL },

	{ "key_min",
	  "minimum size of keys",
	  C_ROW, 0, 10, 32, &g.c_key_min, NULL },

	{ "leaf_page_max",
	  "maximum size of Btree leaf nodes",
	  0, 0, 9, 24, &g.c_leaf_page_max, NULL },

	{ "ops",
	  "the number of modification operations done per run",
	  0, 0, 0, M(2), &g.c_ops, NULL },

	{ "prefix",
	  "if keys are prefix compressed",		/* 80% */
	  C_ROW, C_BOOL, 80, 0, &g.c_prefix, NULL },

	{ "repeat_data_pct",
	  "percent duplicate values in row- or variable-length column-stores",
	  C_ROW|C_VAR, 0, 0, 90, &g.c_repeat_data_pct, NULL },

	{ "reverse",
	  "collate in reverse order",			/* 10% */
	  0, C_BOOL, 10, 0, &g.c_reverse, NULL },

	{ "rows",
	  "the number of rows to create",
	  0, 0, 10, M(1), &g.c_rows, NULL },

	{ "runs",
	  "the number of runs",
	  0, C_IGNORE, 0, UINT_MAX, &g.c_runs, NULL },

	{ "split_pct",
	  "Btree page split size as a percentage of the maximum page size",
	  0, 0, 40, 85, &g.c_split_pct, NULL },

	{ "threads",
	  "the number of threads",
	  0, C_IGNORE, 1, 32, &g.c_threads, NULL },

	{ "value_max",
	  "maximum size of values",
	  C_ROW|C_VAR, 0, 32, 4096, &g.c_value_max, NULL },

	{ "value_min",
	  "minimum size of values",
	  C_ROW|C_VAR, 0, 1, 20, &g.c_value_min, NULL },

	{ "wiredtiger_config",
	  "configuration string used to wiredtiger_open",
	  0, C_IGNORE|C_STRING, 0, 0, NULL, &g.c_config_open },

	{ "write_pct",
	  "percent operations that are writes",
	  0, C_OPS, 0, 90, &g.c_write_pct, NULL },

	{ NULL, NULL, 0, 0, 0, 0, NULL, NULL }
};
