/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Configuration for the wts program is an array of string-based paramters.
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
	uint32_t 	flags;

	uint32_t	 min;			/* Minimum value */
	uint32_t	 max;			/* Maximum value */
	uint32_t	 *v;			/* Value for this run */
} CONFIG;

/*
 * Get a random value between a config min/max pair (inclusive for both min
 * and max).
 */
#define	CONF_RAND(cp)	MMRAND((cp)->min, (cp)->max)

static CONFIG c[] = {
	{ "bitcnt",
	  "number of bits for fixed-length column-store files",
	  C_FIX, 0, 1, 8, &g.c_bitcnt },

	{ "bzip",
	  "if blocks are BZIP2 encoded",		/* 80% */
	  0, C_BOOL, 80, 0, &g.c_bzip },

	{ "cache",
	  "size of the cache in MB",
	  0, 0, 1, 100, &g.c_cache },

	{ "delete_pct",
	  "percent operations that are deletes",
	  0, C_OPS, 0, 45, &g.c_delete_pct },

	{ "file_type",
	  "type of file to create (fix | var | row)",
	  0, C_IGNORE, 1, 3, &g.c_file_type },

	{ "huffman_key",
	  "if keys are huffman encoded",		/* 40% */
	  C_ROW, C_BOOL, 40, 0, &g.c_huffman_key },

	{ "huffman_value",
	 "if values are huffman encoded",		/* 40% */
	 C_ROW|C_VAR, C_BOOL, 40, 0, &g.c_huffman_value },

	{ "insert_pct",
	  "percent operations that are inserts",
	  0, C_OPS, 0, 45, &g.c_insert_pct },

	{ "internal_page_max",
	  "maximum size of Btree internal nodes",
	  0, 0, 9, 17, &g.c_intl_page_max },

	{ "key_max",
	  "maximum size of keys",
	  C_ROW, 0, 64, 128, &g.c_key_max },

	{ "key_min",
	  "minimum size of keys",
	  C_ROW, 0, 10, 32, &g.c_key_min },

	{ "leaf_page_max",
	  "maximum size of Btree leaf nodes",
	  0, 0, 9, 24, &g.c_leaf_page_max },

	{ "ops",
	  "the number of modification operations done per run",
	  0, 0, 0, M(2), &g.c_ops },

	{ "repeat_data_pct",
	  "percent duplicate values in variable-len column-store files",
	  C_VAR, 0, 0, 90, &g.c_repeat_data_pct },

	{ "reverse",
	  "collate in reverse order",			/* 10% */
	  0, C_BOOL, 10, 0, &g.c_reverse },

	{ "rows",
	  "the number of rows to create",
	  0, 0, 10, M(1), &g.c_rows },

	{ "runs",
	  "the number of runs",
	  0, C_IGNORE, 0, UINT_MAX, &g.c_runs },

	{ "threads",
	  "the number of threads",
	  0, C_IGNORE, 1, 32, &g.c_threads },

	{ "value_max",
	  "maximum size of values",
	  C_ROW|C_VAR, 0, 32, 4096, &g.c_value_max },

	{ "value_min",
	  "minimum size of values",
	  C_ROW|C_VAR, 0, 1, 20, &g.c_value_min },

	{ "write_pct",
	  "percent operations that are writes",
	  0, C_OPS, 0, 90, &g.c_write_pct },

	{ NULL, NULL, 0, 0, 0, 0, NULL }
};
