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

#include <sys/stat.h>
#include <sys/time.h>

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
#include <db.h>
#endif
#include <wiredtiger.h>

#include <wiredtiger_ext.h>
extern WT_EXTENSION_API *wt_api;

#define	EXTPATH	"../../ext/"			/* Extensions path */
#define	BZIP_PATH							\
	EXTPATH "compressors/bzip2/.libs/libwiredtiger_bzip2.so"
#define	SNAPPY_PATH							\
	EXTPATH "compressors/snappy/.libs/libwiredtiger_snappy.so"
#define	REVERSE_PATH							\
	EXTPATH "collators/reverse/.libs/libwiredtiger_reverse_collator.so"
#define	KVS_BDB_PATH							\
	EXTPATH "test/kvs_bdb/.libs/libwiredtiger_kvs_bdb.so"

#define	LZO_PATH	".libs/lzo_compress.so"
#define	RAW_PATH	".libs/raw_compress.so"

#define	M(v)		((v) * 1000000)		/* Million */
#define	UNUSED(var)	(void)(var)		/* Quiet unused var warnings */

/* Get a random value between a min/max pair. */
#define	MMRAND(min, max)	(wts_rand() % (((max) + 1) - (min)) + (min))

#define	WT_NAME	"wt"				/* Object name */

#define	RUNDIR		"RUNDIR"		/* Run home */
#define	RUNDIR_KVS	"RUNDIR/KVS"		/* Run home for data-source */

#define	DATASOURCE(v)	(strcmp(v, g.c_data_source) == 0 ? 1 : 0)
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

	char *uri;				/* Object name */

#define	FIX		1			/* File types */
#define	ROW		2
#define	VAR		3
	u_int   type;				/* File type */

#define	COMPRESS_NONE	1
#define	COMPRESS_BZIP	2
#define	COMPRESS_LZO	3
#define	COMPRESS_RAW	4
#define	COMPRESS_SNAPPY	5
	u_int compression;			/* Compression type */

	char *config_open;			/* Command-line configuration */

	u_int c_bitcnt;				/* Config values */
	u_int c_cache;
	char *c_compression;
	char *c_config_open;
	char *c_data_source;
	u_int c_delete_pct;
	u_int c_dictionary;
	char *c_file_type;
	u_int c_huffman_key;
	u_int c_huffman_value;
	u_int c_insert_pct;
	u_int c_internal_key_truncation;
	u_int c_intl_page_max;
	u_int c_key_gap;
	u_int c_key_max;
	u_int c_key_min;
	u_int c_leaf_page_max;
	u_int c_ops;
	u_int c_prefix;
	u_int c_repeat_data_pct;
	u_int c_reverse;
	u_int c_rows;
	u_int c_runs;
	u_int c_split_pct;
	u_int c_threads;
	u_int c_value_max;
	u_int c_value_min;
	u_int c_write_pct;

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

void	 config_clear(void);
void	 config_error(void);
void	 config_file(const char *);
void	 config_print(int);
void	 config_setup(void);
void	 config_single(const char *, int);
void	 die(int, const char *, ...);
void	 key_len_setup(void);
void	 key_gen_setup(uint8_t **);
void	 key_gen(uint8_t *, uint32_t *, uint64_t, int);
void	 syserr(const char *);
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

void	 wiredtiger_kvs_stec_close(WT_CONNECTION *);
void	 wiredtiger_kvs_stec_init(WT_CONNECTION *);
