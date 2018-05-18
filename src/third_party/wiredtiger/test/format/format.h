/*-
 * Public Domain 2014-2018 MongoDB, Inc.
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
 */

#include "test_util.h"

#ifdef BDB
/*
 * Berkeley DB has an #ifdef we need to provide a value for, we'll see an
 * undefined error if it's unset during a strict compile.
 */
#ifndef	DB_DBM_HSEARCH
#define	DB_DBM_HSEARCH	0
#endif
#include <assert.h>
#include <db.h>
#endif

#define	EXTPATH	"../../ext/"			/* Extensions path */

#define	LZ4_PATH							\
	EXTPATH "compressors/lz4/.libs/libwiredtiger_lz4.so"
#define	SNAPPY_PATH							\
	EXTPATH "compressors/snappy/.libs/libwiredtiger_snappy.so"
#define	ZLIB_PATH							\
	EXTPATH "compressors/zlib/.libs/libwiredtiger_zlib.so"
#define	ZSTD_PATH							\
	EXTPATH "compressors/zstd/.libs/libwiredtiger_zstd.so"

#define	REVERSE_PATH							\
	EXTPATH "collators/reverse/.libs/libwiredtiger_reverse_collator.so"

#define	ROTN_PATH							\
	EXTPATH "encryptors/rotn/.libs/libwiredtiger_rotn.so"

#define	KVS_BDB_PATH							\
	EXTPATH "test/kvs_bdb/.libs/libwiredtiger_kvs_bdb.so"
#define	HELIUM_PATH							\
	EXTPATH "datasources/helium/.libs/libwiredtiger_helium.so"

#define	LZO_PATH	".libs/lzo_compress.so"

#undef	M
#define	M(v)		((v) * 1000000)		/* Million */
#undef	KILOBYTE
#define	KILOBYTE(v)	((v) * 1024)
#undef	MEGABYTE
#define	MEGABYTE(v)	((v) * 1048576)

#define	WT_NAME	"wt"				/* Object name */

#define	DATASOURCE(v)	(strcmp(v, g.c_data_source) == 0 ? 1 : 0)
#define	SINGLETHREADED	(g.c_threads == 1)

#define	FORMAT_OPERATION_REPS	3		/* 3 thread operations sets */

#define	MAX_MODIFY_ENTRIES	5		/* maximum change vectors */

typedef struct {
	char *home;				/* Home directory */
	char *home_backup;			/* Hot-backup directory */
	char *home_backup_init;			/* Initialize backup command */
	char *home_bdb;				/* BDB directory */
	char *home_config;			/* Run CONFIG file path */
	char *home_init;			/* Initialize home command */
	char *home_log;				/* Operation log file path */
	char *home_rand;			/* RNG log file path */
	char *home_salvage_copy;		/* Salvage copy command */
	char *home_stats;			/* Statistics file path */

	char *helium_mount;			/* Helium volume */

	char wiredtiger_open_config[8 * 1024];	/* Database open config */

#ifdef HAVE_BERKELEY_DB
	void *bdb;				/* BDB comparison handle */
	void *dbc;				/* BDB cursor handle */
#endif

	WT_CONNECTION	 *wts_conn;
	WT_EXTENSION_API *wt_api;

	bool  rand_log_stop;			/* Logging turned off */
	FILE *randfp;				/* Random number log */

	uint32_t run_cnt;			/* Run counter */

	enum {
	    LOG_FILE=1,				/* Use a log file */
	    LOG_OPS=2				/* Log all operations */
	} logging;
	FILE *logfp;				/* Log file */

	bool replay;				/* Replaying a run. */
	bool workers_finished;			/* Operations completed */

	pthread_rwlock_t backup_lock;		/* Backup running */

	WT_RAND_STATE rnd;			/* Global RNG state */

	pthread_rwlock_t prepare_lock;		/* Prepare running */

	uint64_t timestamp;			/* Counter for timestamps */

	uint64_t truncate_cnt;			/* Counter for truncation */

	/*
	 * We have a list of records that are appended, but not yet "resolved",
	 * that is, we haven't yet incremented the g.rows value to reflect the
	 * new records.
	 */
	uint64_t *append;			/* Appended records */
	size_t    append_max;			/* Maximum unresolved records */
	size_t	  append_cnt;			/* Current unresolved records */
	pthread_rwlock_t append_lock;		/* Single-thread resolution */

	pthread_rwlock_t death_lock;		/* Single-thread failure */

	char *uri;				/* Object name */

	char *config_open;			/* Command-line configuration */

	uint32_t c_abort;			/* Config values */
	uint32_t c_alter;
	uint32_t c_auto_throttle;
	uint32_t c_backups;
	uint32_t c_bitcnt;
	uint32_t c_bloom;
	uint32_t c_bloom_bit_count;
	uint32_t c_bloom_hash_count;
	uint32_t c_bloom_oldest;
	uint32_t c_cache;
	uint32_t c_cache_minimum;
	char	*c_checkpoint;
	uint32_t c_checkpoint_log_size;
	uint32_t c_checkpoint_wait;
	char	*c_checksum;
	uint32_t c_chunk_size;
	uint32_t c_compact;
	char	*c_compression;
	char	*c_config_open;
	uint32_t c_data_extend;
	char	*c_data_source;
	uint32_t c_delete_pct;
	uint32_t c_dictionary;
	uint32_t c_direct_io;
	char	*c_encryption;
	uint32_t c_evict_max;
	char	*c_file_type;
	uint32_t c_firstfit;
	uint32_t c_huffman_key;
	uint32_t c_huffman_value;
	uint32_t c_in_memory;
	uint32_t c_independent_thread_rng;
	uint32_t c_insert_pct;
	uint32_t c_internal_key_truncation;
	uint32_t c_intl_page_max;
	char	*c_isolation;
	uint32_t c_key_gap;
	uint32_t c_key_max;
	uint32_t c_key_min;
	uint32_t c_leaf_page_max;
	uint32_t c_leak_memory;
	uint32_t c_logging;
	uint32_t c_logging_archive;
	char	*c_logging_compression;
	uint32_t c_logging_file_max;
	uint32_t c_logging_prealloc;
	uint32_t c_long_running_txn;
	uint32_t c_lsm_worker_threads;
	uint32_t c_merge_max;
	uint32_t c_mmap;
	uint32_t c_modify_pct;
	uint32_t c_ops;
	uint32_t c_prefix_compression;
	uint32_t c_prefix_compression_min;
	uint32_t c_prepare;
	uint32_t c_quiet;
	uint32_t c_read_pct;
	uint32_t c_rebalance;
	uint32_t c_repeat_data_pct;
	uint32_t c_reverse;
	uint32_t c_rows;
	uint32_t c_runs;
	uint32_t c_salvage;
	uint32_t c_split_pct;
	uint32_t c_statistics;
	uint32_t c_statistics_server;
	uint32_t c_threads;
	uint32_t c_timer;
	uint32_t c_timing_stress_checkpoint;
	uint32_t c_timing_stress_split_1;
	uint32_t c_timing_stress_split_2;
	uint32_t c_timing_stress_split_3;
	uint32_t c_timing_stress_split_4;
	uint32_t c_timing_stress_split_5;
	uint32_t c_timing_stress_split_6;
	uint32_t c_timing_stress_split_7;
	uint32_t c_truncate;
	uint32_t c_txn_freq;
	uint32_t c_txn_timestamps;
	uint32_t c_value_max;
	uint32_t c_value_min;
	uint32_t c_verify;
	uint32_t c_write_pct;

#define	FIX				1
#define	ROW				2
#define	VAR				3
	u_int type;				/* File type's flag value */

#define	CHECKPOINT_OFF			1
#define	CHECKPOINT_ON			2
#define	CHECKPOINT_WIREDTIGER		3
	u_int c_checkpoint_flag;		/* Checkpoint flag value */

#define	CHECKSUM_OFF			1
#define	CHECKSUM_ON			2
#define	CHECKSUM_UNCOMPRESSED		3
	u_int c_checksum_flag;			/* Checksum flag value */

#define	COMPRESS_NONE			1
#define	COMPRESS_LZ4			2
#define	COMPRESS_LZ4_NO_RAW		3
#define	COMPRESS_LZO			4
#define	COMPRESS_SNAPPY			5
#define	COMPRESS_ZLIB			6
#define	COMPRESS_ZLIB_NO_RAW		7
#define	COMPRESS_ZSTD			8
	u_int c_compression_flag;		/* Compression flag value */
	u_int c_logging_compression_flag;	/* Log compression flag value */

#define	ENCRYPT_NONE			1
#define	ENCRYPT_ROTN_7			2
	u_int c_encryption_flag;		/* Encryption flag value */

#define	ISOLATION_RANDOM		1
#define	ISOLATION_READ_UNCOMMITTED	2
#define	ISOLATION_READ_COMMITTED	3
#define	ISOLATION_SNAPSHOT		4
	u_int c_isolation_flag;			/* Isolation flag value */

	uint32_t intl_page_max;			/* Maximum page sizes */
	uint32_t leaf_page_max;

	uint64_t key_cnt;			/* Keys loaded so far */
	uint64_t rows;				/* Total rows */

	uint32_t key_rand_len[1031];		/* Key lengths */
} GLOBAL;
extern GLOBAL g;

typedef struct {
	int	    id;				/* simple thread ID */
	wt_thread_t tid;			/* thread ID */

	WT_RAND_STATE rnd;			/* thread RNG state */

	uint64_t commit_timestamp;		/* last committed timestamp */
	uint64_t read_timestamp;		/* read timestamp */

	volatile bool quit;			/* thread should quit */

	uint64_t ops;				/* total operations */
	uint64_t commit;			/* operation counts */
	uint64_t insert;
	uint64_t prepare;
	uint64_t remove;
	uint64_t rollback;
	uint64_t search;
	uint64_t truncate;
	uint64_t update;

	uint64_t keyno;				/* key */
	WT_ITEM	 *key, _key;			/* key, value */
	WT_ITEM	 *value, _value;

	uint64_t last;				/* truncate range */
	WT_ITEM	 *lastkey, _lastkey;

	WT_ITEM  *tbuf, _tbuf;			/* temporary buffer */

#define	TINFO_RUNNING	1			/* Running */
#define	TINFO_COMPLETE	2			/* Finished */
#define	TINFO_JOINED	3			/* Resolved */
	volatile int state;			/* state */
} TINFO;

#ifdef HAVE_BERKELEY_DB
void	 bdb_close(void);
void	 bdb_insert(const void *, size_t, const void *, size_t);
void	 bdb_np(bool, void *, size_t *, void *, size_t *, int *);
void	 bdb_open(void);
void	 bdb_read(uint64_t, void *, size_t *, int *);
void	 bdb_remove(uint64_t, int *);
void	 bdb_truncate(uint64_t, uint64_t);
void	 bdb_update(const void *, size_t, const void *, size_t);
#endif

WT_THREAD_RET alter(void *);
WT_THREAD_RET backup(void *);
WT_THREAD_RET checkpoint(void *);
WT_THREAD_RET compact(void *);
void	 config_clear(void);
void	 config_error(void);
void	 config_file(const char *);
void	 config_print(int);
void	 config_setup(void);
void	 config_single(const char *, int);
void	 fclose_and_clear(FILE **);
void	 key_gen(WT_ITEM *, uint64_t);
void	 key_gen_init(WT_ITEM *);
void	 key_gen_insert(WT_RAND_STATE *, WT_ITEM *, uint64_t);
void	 key_gen_teardown(WT_ITEM *);
void	 key_init(void);
WT_THREAD_RET lrt(void *);
void	 path_setup(const char *);
void	 print_item(const char *, WT_ITEM *);
void	 print_item_data(const char *, const uint8_t *, size_t);
int	 read_row_worker(WT_CURSOR *, uint64_t, WT_ITEM *, WT_ITEM *, bool);
uint32_t rng(WT_RAND_STATE *);
WT_THREAD_RET timestamp(void *);
void	 track(const char *, uint64_t, TINFO *);
void	 val_gen(WT_RAND_STATE *, WT_ITEM *, uint64_t);
void	 val_gen_init(WT_ITEM *);
void	 val_gen_teardown(WT_ITEM *);
void	 val_init(void);
void	 val_teardown(void);
void	 wts_close(void);
void	 wts_dump(const char *, int);
void	 wts_init(void);
void	 wts_load(void);
void	 wts_open(const char *, bool, WT_CONNECTION **);
void	 wts_ops(int);
void	 wts_read_scan(void);
void	 wts_rebalance(void);
void	 wts_reopen(void);
void	 wts_salvage(void);
void	 wts_stats(void);
void	 wts_verify(const char *);

/*
 * mmrand --
 *	Return a random value between a min/max pair, inclusive.
 */
static inline uint32_t
mmrand(WT_RAND_STATE *rnd, u_int min, u_int max)
{
	uint32_t v;
	u_int range;

	v = rng(rnd);
	range = (max - min) + 1;
	v %= range;
	v += min;
	return (v);
}
