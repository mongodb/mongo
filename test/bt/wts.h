/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
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
#include "wiredtiger.h"
#endif

/* General purpose. */
#define	KB(v)	((v) * 1024)			/* Kilobytes */
#define	M(v)	((v) * 1000000)			/* Million */
#define	MB(v)	((v) * 1024 * 1024)		/* Megabytes */

/* Get a random value between a min/max pair. */
#define	MMRAND(min, max)	(rand() % ((max + 1) - (min)) + (min))

#define	FIX		1			/* Database types */
#define	ROW		2
#define	VAR		3

#define	WT_PREFIX	"wt"			/* Output file prefix */
#define	BDB_PREFIX	"bdb"

typedef struct {
	char *progname;				/* Program name */

	void *bdb_db;				/* BDB DB handle */

	void *wts_db;				/* WT DB handle */
	FILE *logfp;				/* WT log file stream */

	enum                                    /* Dumps */
	    { DUMP_DEBUG=1, DUMP_PRINT=2 } dump;

	u_int32_t run_cnt;			/* Run counter */

	int stats;				/* Statistics */

	int verbose;				/* Verbosity */

	u_int32_t c_bulk_cnt;			/* Config values */
	u_int32_t c_cache;
	u_int32_t c_data_max;
	u_int32_t c_data_min;
	u_int32_t c_database_type;
	u_int32_t c_duplicates;
	u_int32_t c_duplicates_pct;
	u_int32_t c_huffman_data;
	u_int32_t c_huffman_key;
	u_int32_t c_internal_node;
	u_int32_t c_key_cnt;
	u_int32_t c_key_max;
	u_int32_t c_key_min;
	u_int32_t c_leaf_node;
	u_int32_t c_rand_seed;
	u_int32_t c_read_ops;
	u_int32_t c_repeat_comp;
	u_int32_t c_repeat_comp_pct;
	u_int32_t c_runs;
	u_int32_t c_write_ops;

	u_int32_t key_cnt;			/* Current key count */
	u_int16_t key_rand_len[1031];		/* Key lengths */
} GLOBAL;
extern GLOBAL g;

void	 bdb_insert(void *, u_int32_t, void *, u_int32_t);
int	 bdb_read_key(void *, u_int32_t, void *, u_int32_t *);
int	 bdb_read_recno(u_int64_t, void *, u_int32_t *, void *, u_int32_t *);
void	 bdb_setup(int);
void	 bdb_teardown(void);
void	 config_dump(int);
void	 config_file(char *);
void	 config(void);
void	 config_names(void);
void	 config_single(char *);
void	 data_gen(DBT *);
char	*fname(const char *, const char *);
void	 key_gen(DBT *, u_int64_t);
void	 track(const char *, u_int64_t);
int	 wts_bulk_load(void);
int	 wts_read_key(void);
int	 wts_read_recno(void);
int	 wts_setup(int, int);
void	 wts_teardown(void);
