/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef BDB
#include "build_unix/db.h"
#else
#include <wiredtiger.h>
#endif

#define	M(v)		((v) * 1000000)		/* Million */
#define	UNUSED(var)	(void)(var)		/* Quiet unused var warnings */

#define	FIX		1			/* File types */
#define	ROW		2
#define	VAR		3

/* Get a random value between a min/max pair. */
#define	MMRAND(min, max)	(wts_rand() % (((max) + 1) - (min)) + (min))

#define	WT_TABLENAME	"file:__wt"

#define	SINGLETHREADED	(g.c_threads == 1)

typedef struct {
	char *progname;				/* Program name */

	void *bdb;				/* BDB comparison handle */
	void *dbc;				/* BDB cursor handle */

	void *wts_conn;				/* WT_CONNECTION handle */

	FILE *rand_log;				/* Random number log */

	uint32_t run_cnt;			/* Run counter */

	enum {
	    LOG_FILE=1,				/* Use a log file */
	    LOG_OPS=2				/* Log all operations */
	} logging;
	FILE *logfp;				/* Log file */

	int replay;				/* Replaying a run. */
	int track;				/* Track progress */

	char *config_open;			/* Command-line configuration */

	uint32_t c_bitcnt;			/* Config values */
	uint32_t c_bzip;
	uint32_t c_cache;
	uint32_t c_delete_pct;
	uint32_t c_file_type;
	uint32_t c_huffman_key;
	uint32_t c_huffman_value;
	uint32_t c_insert_pct;
	uint32_t c_intl_page_max;
	uint32_t c_key_max;
	uint32_t c_key_min;
	uint32_t c_leaf_page_max;
	uint32_t c_ops;
	uint32_t c_repeat_data_pct;
	uint32_t c_reverse;
	uint32_t c_rows;
	uint32_t c_runs;
	uint32_t c_threads;
	uint32_t c_value_max;
	uint32_t c_value_min;
	uint32_t c_write_pct;

	uint32_t key_cnt;			/* Keys loaded so far */
	uint32_t rows;				/* Total rows */
	uint16_t key_rand_len[1031];		/* Key lengths */
} GLOBAL;
extern GLOBAL g;

typedef struct {
	uint64_t search;
	uint64_t insert;
	uint64_t update;
	uint64_t remove;

	int       id;					/* simple thread ID */
	pthread_t tid;					/* thread ID */

#define	TINFO_RUNNING	1				/* Running */
#define	TINFO_COMPLETE	2				/* Finished */
#define	TINFO_JOINED	3				/* Resolved */
	volatile int state;				/* state */
} TINFO;

void	 bdb_close(void);
void	 bdb_insert(const void *, uint32_t, const void *, uint32_t);
void	 bdb_np(int, void *, uint32_t *, void *, uint32_t *, int *);
void	 bdb_open(void);
void	 bdb_read(uint64_t, void *, uint32_t *, int *);
void	 bdb_remove(uint64_t, int *);
void	 bdb_update(const void *, uint32_t, const void *, uint32_t, int *);

void	 config_error(void);
const char *
	 config_dtype(void);
void	 config_file(const char *);
void	 config_print(int);
void	 config_setup(void);
void	 config_single(const char *, int);
void	 die(int, const char *, ...);
void	 key_len_setup(void);
void	 key_gen_setup(uint8_t **);
void	 key_gen(uint8_t *, uint32_t *, uint64_t, int);
void	 track(const char *, uint64_t, TINFO *);
void	 val_gen_setup(uint8_t **);
void	 value_gen(uint8_t *, uint32_t *, uint64_t);
void	 wts_close(void);
void	 wts_dump(const char *, int);
void	 wts_load(void);
void	 wts_open(void);
void	 wts_ops(void);
uint32_t wts_rand(void);
void	 wts_read_scan(void);
void	 wts_salvage(void);
void	 wts_stats(void);
void	 wts_verify(const char *);
