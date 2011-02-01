/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef BDB
#include "build_unix/db.h"
#else
#include "wt_internal.h"
#endif

/* General purpose. */
#define	KB(v)	((v) * 1024)			/* Kilobytes */
#define	M(v)	((v) * 1000000)			/* Million */
#define	MB(v)	((v) * 1024 * 1024)		/* Megabytes */

/* Get a random value between a min/max pair. */
#define	MMRAND(min, max)	(wts_rand() % ((max + 1) - (min)) + (min))

#define	FIX		1			/* Database types */
#define	ROW		2
#define	VAR		3

#define	WT_PREFIX	"wt"			/* Output file prefix */
#define	BDB_PREFIX	"bdb"

typedef struct {
	char *progname;				/* Program name */

	void *bdb_db;				/* BDB DB handle */

	void *wts_db;				/* WT DB handle */
	void *wts_toc;				/* WT WT_TOC handle */
	FILE *wts_log;				/* WT log file stream */

	FILE *rand_log;				/* Random number log */

	u_int32_t run_cnt;			/* Run counter */

	int replay;				/* Replaying a run. */
	int verbose;				/* Verbosity */

	char *key_gen_buf;

	u_int32_t c_cache;			/* Config values */
	u_int32_t c_data_max;
	u_int32_t c_data_min;
	u_int32_t c_database_type;
	u_int32_t c_delete_pct;
	u_int32_t c_duplicates_pct;
	u_int32_t c_huffman_data;
	u_int32_t c_huffman_key;
	u_int32_t c_intl_node_max;
	u_int32_t c_intl_node_min;
	u_int32_t c_key_cnt;
	u_int32_t c_key_max;
	u_int32_t c_key_min;
	u_int32_t c_leaf_node_max;
	u_int32_t c_leaf_node_min;
	u_int32_t c_ops;
	u_int32_t c_repeat_comp_pct;
	u_int32_t c_rows;
	u_int32_t c_runs;
	u_int32_t c_write_pct;

	u_int32_t key_cnt;			/* Keys loaded so far */
	u_int16_t key_rand_len[1031];		/* Key lengths */
} GLOBAL;
extern GLOBAL g;

int	 bdb_del(u_int64_t, int *);
void	 bdb_insert(void *, u_int32_t, void *, u_int32_t);
int	 bdb_put(u_int64_t, void *, u_int32_t, int *);
int	 bdb_read(u_int64_t, void *, u_int32_t *, int *);
void	 bdb_startup(void);
void	 bdb_teardown(void);
void	 config_dump(int);
void	 config_file(char *);
void	 config_names(void);
void	 config_setup(void);
void	 config_single(char *);
void	 data_gen(DBT *, int);
char	*fname(const char *);
void	 key_gen(DBT *, u_int64_t);
void	 key_gen_setup(void);
void	 track(const char *, u_int64_t);
int	 wts_bulk_load(void);
int	 wts_del(u_int64_t);
int	 wts_dump(void);
int	 wts_ops(void);
int	 wts_rand(void);
int	 wts_read_col_scan(void);
int	 wts_read_row_scan(void);
int	 wts_startup(int);
int	 wts_stats(void);
void	 wts_teardown(void);
int	 wts_verify(void);
