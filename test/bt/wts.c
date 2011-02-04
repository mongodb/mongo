/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

extern void __wt_debug_dbt(const char *, void *, FILE *);

static int cb_bulk(DB *, DBT **, DBT **);
static int wts_del_col(u_int64_t);
static int wts_del_row(u_int64_t);
static int wts_notfound_chk(ENV *, const char *, int, int, u_int64_t);
static int wts_put_col(u_int64_t);
static int wts_put_row(u_int64_t);
static int wts_read_col(u_int64_t);
static int wts_read_row(u_int64_t);
static int wts_sync(void);

int
wts_startup(int logfile)
{
	time_t now;
	DB *db;
	ENV *env;
	WT_TOC *toc;
	u_int32_t intl_node_max, intl_node_min, leaf_node_max, leaf_node_min;
	int ret;
	char *p;

	if (logfile) {
		if (g.wts_log != NULL) {
			(void)fclose(g.wts_log);
			g.wts_log = NULL;
		}

		p = fname("log");
		if ((g.wts_log = fopen(p, "w")) == NULL) {
			fprintf(stderr,
			    "%s: %s: %s\n", g.progname, p, strerror(errno));
			exit (EXIT_FAILURE);
		}
	}

	if (g.wts_log != NULL) {
		fprintf(
		    g.wts_log, "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
		(void)time(&now);
		fprintf(g.wts_log, "WT startup: %s", ctime(&now));
		fprintf(
		    g.wts_log, "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
	}

	if ((ret = wiredtiger_simple_setup(
	    g.progname, &db, g.c_cache, WT_MEMORY_CHECK)) != 0) {
		fprintf(stderr, "%s: wiredtiger_simple_setup: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	env = db->env;

	(void)env->errpfx_set(env, g.progname);
	(void)env->errfile_set(env, stderr);

	/* Open the log file. */
	if (logfile) {
		(void)env->verbose_set(env,
		    // WT_VERB_FILEOPS |
		    // WT_VERB_HAZARD |
		    // WT_VERB_MUTEX |
		    // WT_VERB_READ |
		    // WT_VERB_EVICT |
		    0);
		(void)env->msgfile_set(env, g.wts_log);
	}

	intl_node_max = (u_int)1 << g.c_intl_node_max;
	intl_node_min = (u_int)1 << g.c_intl_node_min;
	leaf_node_max = (u_int)1 << g.c_leaf_node_max;
	leaf_node_min = (u_int)1 << g.c_leaf_node_min;
	if ((ret = db->btree_pagesize_set(db, 0,
	    intl_node_min, intl_node_max, leaf_node_min, leaf_node_max)) != 0) {
		db->err(db, ret, "Db.btree_pagesize_set");
		return (1);
	}

	switch (g.c_database_type) {
	case FIX:
		/*
		 * XXX
		 * Don't go past the WT limit of 20 objects per leaf page.
		 */
		if (20 * g.c_data_min > leaf_node_min)
			g.c_data_min = leaf_node_min / 20;
		if ((ret = db->column_set(db, g.c_data_min,
		    NULL, g.c_repeat_comp_pct != 0 ? WT_RLE : 0)) != 0) {
			db->err(db, ret, "Db.column_set");
			return (1);
		}
		break;
	case VAR:
		if ((ret = db->column_set(db, 0, NULL, 0)) != 0) {
			db->err(db, ret, "Db.column_set");
			return (1);
		}
		/* FALLTHROUGH */
	case ROW:
		if (g.c_huffman_key && (ret = db->huffman_set(
		    db, NULL, 0, WT_ASCII_ENGLISH|WT_HUFFMAN_KEY)) != 0) {
			db->err(db, ret, "Db.huffman_set: data");
			return (1);
		}
		if (g.c_huffman_data && (ret = db->huffman_set(
		    db, NULL, 0, WT_ASCII_ENGLISH|WT_HUFFMAN_DATA)) != 0) {
			db->err(db, ret, "Db.huffman_set: data");
			return (1);
		}
		break;
	}

	p = fname("wt");
	if ((ret = db->open(db, p, 0660, WT_CREATE)) != 0) {
		db->err(db, ret, "Db.open: %s", p);
		return (1);
	}

	if ((ret = db->env->toc(db->env, 0, &toc)) != 0) {
		db->err(db, ret, "Env.toc");
		return (1);
	}

	g.wts_db = db;
	g.wts_toc = toc;
	return (0);
}

void
wts_teardown()
{
	WT_TOC *toc;
	time_t now;

	toc = g.wts_toc;

	if (g.wts_log != NULL) {
		fprintf(
		    g.wts_log, "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
		(void)time(&now);
		fprintf(g.wts_log, "WT teardown: %s", ctime(&now));
		fprintf(
		    g.wts_log, "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
	}

	assert(wts_sync() == 0);
	assert(toc->close(toc, 0) == 0);
	assert(wiredtiger_simple_teardown(g.progname, g.wts_db) == 0);
}

int
wts_bulk_load()
{
	DB *db;
	int ret;

	db = g.wts_db;

	switch (g.c_database_type) {
	case FIX:
	case VAR:
		ret = db->bulk_load(db, 0, track, cb_bulk);
		break;
	case ROW:
		ret = db->bulk_load(db, WT_DUPLICATES, track, cb_bulk);
		break;
	}
	if (ret != 0) {
		db->err(db, ret, "Db.bulk_load");
		return (1);
	}
	return (0);
}

int
wts_dump()
{
	DB *db;
	FILE *fp;
	int ret;
	char *p;

	db = g.wts_db;

	/* Dump the WiredTiger database. */
	track("dump", (u_int64_t)0);
	p = fname("wt_dump");
	if ((fp = fopen(p, "w")) == NULL) {
		db->err(db, errno, "fopen: %s", p);
		return (1);
	}
	if ((ret = db->dump(db, fp, track, WT_PRINTABLES)) != 0) {
		db->err(db, ret, "Db.dump");
		return (1);
	}
	(void)fclose(fp);

	track("dump comparison", (u_int64_t)0);
	switch (g.c_database_type) {
	case FIX:
	case VAR:
		p = "sh ./s_dumpcmp -c";
		break;
	case ROW:
		p = "sh ./s_dumpcmp";
		break;
	}
	if (system(p) != 0) {
		db->errx(db, "dump comparison failed");
		return (1);
	}

	return (0);
}

static int
wts_sync()
{
	DB *db;
	int ret;

	db = g.wts_db;

	if ((ret = db->sync(db, track, WT_OSWRITE)) != 0) {
		db->err(db, ret, "Db.sync");
		return (1);
	}
	return (0);
}

int
wts_verify()
{
	DB *db;
	int ret;

	db = g.wts_db;

	if ((ret = db->verify(db, track, 0)) != 0) {
		db->err(db, ret, "Db.verify");
		return (1);
	}
	return (0);
}

/*
 * wts_stats --
 *	Dump the run's statistics.
 */
int
wts_stats()
{
	DB *db;
	FILE *fp;
	char *p;
	int ret;

	db = g.wts_db;

	track("stat", (u_int64_t)0);
	p = fname("stats");
	if ((fp = fopen(p, "w")) == NULL) {
		db->err(db, errno, "fopen: %s", p);
		return (1);
	}
	if ((ret = db->env->stat_print(db->env, fp, 0)) != 0) {
		db->err(db, ret, "Env.stat_print");
		return (1);
	}
	(void)fclose(fp);

	return (0);
}

/*
 * cb_bulk --
 *	WiredTiger bulk load callback routine. 
 */
static int
cb_bulk(DB *db, DBT **keyp, DBT **datap)
{
	static DBT key, data;

	db = NULL;					/* Lint */
	++g.key_cnt;

	if (g.key_cnt > g.c_rows) {
		g.key_cnt = g.c_rows;
		return (1);
	}

	/*
	 * Generate a set of duplicates for each key if duplicates have
	 * been configured.  The duplicate_pct configuration is a
	 * percentage, which defines the number of keys that get
	 * duplicate data items, and the number of duplicate data items
	 * for each such key is a random value in-between 2 and the
	 * value of duplicate_cnt.
	 */
	if (g.key_cnt == 1 || g.c_duplicates_pct == 0 ||
	    (u_int32_t)wts_rand() % 100 > g.c_duplicates_pct)
		key_gen(&key, g.key_cnt);
	data_gen(&data, 1);

	switch (g.c_database_type) {
	case FIX:
	case VAR:
		*keyp = NULL;
		break;
	case ROW:
		*keyp = &key;
		if (g.wts_log != NULL)
			fprintf(g.wts_log, "load key {%.*s}\n",
			    (int)key.size, (char *)key.data);
		break;
	}
	*datap = &data;
	if (g.wts_log != NULL)
		fprintf(g.wts_log,
		    "load data {%.*s}\n", (int)data.size, (char *)data.data);

	/* Insert the item into BDB. */
	bdb_insert(key.data, key.size, data.data, data.size);

	return (0);
}

/*
 * wts_ops --
 *	Perform a number of operations.
 */
int
wts_ops()
{
	u_int64_t keyno;
	u_int cnt;
	int op;

	for (cnt = 0; cnt < g.c_ops; ++cnt) {
		keyno = MMRAND(1, g.c_rows);

		/*
		 * Perform some number of read/write/delete operations; the
		 * number of deletes and writes are specified, reads are the
		 * rest.  A read operation always follows a delete or write
		 * operation to confirm it worked.
		 */
		op = wts_rand() % 100;
		if ((u_int32_t)op < g.c_delete_pct) {
			switch (g.c_database_type) {
			case ROW:
				if (wts_del_row(keyno))
					return (1);
				break;
			case FIX:
			case VAR:
				if (wts_del_col(keyno))
					return (1);
				break;
			}
		} else if ((u_int32_t)op < g.c_delete_pct + g.c_write_pct) {
			switch (g.c_database_type) {
			case ROW:
				if (wts_put_row(keyno))
					return (1);
				break;
			case FIX:
			case VAR:
				if (wts_put_col(keyno))
					return (1);
				break;
			}
		}

		switch (g.c_database_type) {
		case ROW:
			if (wts_read_row(keyno))
				return (1);
			break;
		case FIX:
		case VAR:
			if (wts_read_col(keyno))
				return (1);
			break;
		}

		if (cnt % 10 == 0)
			track("read/write ops", (u_int64_t)cnt);
	}
	return (0);
}

/*
 * wts_read_key_scan --
 *	Read and verify elements in a row database.
 */
int
wts_read_row_scan()
{
	u_int64_t cnt, last_cnt;

	/* Check a random subset of the records using the key. */
	for (last_cnt = cnt = 0; cnt < g.key_cnt;) {
		cnt += wts_rand() % 17 + 1;
		if (cnt > g.c_rows)
			cnt = g.c_rows;
		if (cnt - last_cnt > 1000) {
			track("read row scan", cnt);
			last_cnt = cnt;
		}

		if (wts_read_row(cnt))
			return (1);
	}
	return (0);
}

#define	NTF_CHK(a) do {							\
	switch (a) {							\
	case 0:								\
		break;							\
	case 1:								\
		return (1);						\
	case 2:								\
		return (0);						\
	}								\
} while (0)

/*
 * wts_read_row --
 *	Read and verify a single element in a row database.
 */
static int
wts_read_row(u_int64_t keyno)
{
	static DBT key, data, bdb_data;
	DB *db;
	ENV *env;
	WT_TOC *toc;
	int notfound, ret;

	db = g.wts_db;
	toc = g.wts_toc;
	env = db->env;

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log, "read %llu\n", (unsigned long long)keyno);

	/* Retrieve the BDB data item. */
	if (bdb_read(keyno, &bdb_data.data, &bdb_data.size, &notfound))
		return (1);

	/* Retrieve the key/data pair by key. */
	key_gen(&key, keyno);
	if ((ret = db->row_get(
	    db, toc, &key, &data, 0)) != 0 && ret != WT_NOTFOUND) {
		env->err(env, ret,
		    "wts_read_key: read row %llu by key",
		    (unsigned long long)keyno);
		return (1);
	}

	/* Check for not-found status. */
	NTF_CHK(wts_notfound_chk(env, "wts_read_row", ret, notfound, keyno));

	/* Compare the two. */
	if (data.size != bdb_data.size ||
	    memcmp(data.data, bdb_data.data, data.size) != 0) {
		fprintf(stderr,
		    "wts_read_key: read row %llu by key:\n",
		    (unsigned long long)keyno);
		__wt_debug_dbt("\tbdb", &bdb_data, stderr);
		__wt_debug_dbt("\twt", &data, stderr);
		return (1);
	}
	return (0);
}

/*
 * wts_read_col_scan --
 *	Read and verify elements in a column database.
 */
int
wts_read_col_scan()
{
	u_int64_t cnt, last_cnt;

	/* Check a random subset of the records using the record number. */
	for (last_cnt = cnt = 0; cnt < g.c_rows;) {
		cnt += wts_rand() % 17 + 1;
		if (cnt > g.c_rows)
			cnt = g.c_rows;
		if (cnt - last_cnt > 1000) {
			track("read column scan", cnt);
			last_cnt = cnt;
		}

		if (wts_read_col(cnt))
			return (1);
	}
	return (0);
}

/*
 * wts_read_col --
 *	Read and verify a single element in a column database.
 */
static int
wts_read_col(u_int64_t keyno)
{
	static DBT data, bdb_data;
	DB *db;
	ENV *env;
	WT_TOC *toc;
	int notfound, ret;

	db = g.wts_db;
	toc = g.wts_toc;
	env = db->env;

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log, "read %llu\n", (unsigned long long)keyno);

	/* Retrieve the BDB data item. */
	if (bdb_read(keyno, &bdb_data.data, &bdb_data.size, &notfound))
		return (1);

	/* Retrieve the key/data pair by record number. */
	if ((ret = db->col_get(
	    db, toc, keyno, &data, 0)) != 0 && ret != WT_NOTFOUND) {
		env->err(env, ret,
		    "wts_read_recno: read column %llu by recno",
		    (unsigned long long)keyno);
		return (1);
	}

	/* Check for not-found status. */
	NTF_CHK(wts_notfound_chk(env, "wts_read_col", ret, notfound, keyno));

	/* Compare the two. */
	if (data.size != bdb_data.size ||
	    memcmp(data.data, bdb_data.data, data.size) != 0) {
		fprintf(stderr,
		    "wts_read_recno: read column %llu by recno:\n",
		    (unsigned long long)keyno);
		__wt_debug_dbt("\tbdb data", &bdb_data, stderr);
		__wt_debug_dbt("\t wt data", &data, stderr);
		return (1);
	}

	return (0);
}

/*
 * wts_put_row --
 *	Replace an element in a row database.
 */
static int
wts_put_row(u_int64_t keyno)
{
	static DBT key, data;
	DB *db;
	ENV *env;
	WT_TOC *toc;
	int notfound, ret;

	db = g.wts_db;
	toc = g.wts_toc;
	env = db->env;

	key_gen(&key, keyno);
	data_gen(&data, 0);

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log, "put %llu {%.*s}\n",
		    (unsigned long long)keyno,
		    (int)data.size, (char *)data.data);

	if (bdb_put(keyno, data.data, data.size, &notfound))
		return (1);

	if ((ret = db->row_put(
	    db, toc, &key, &data, 0)) != 0 && ret != WT_NOTFOUND) {
		env->err(env, ret, "wts_put_row: put row %llu by key",
		    (unsigned long long)keyno);
		return (1);
	}
	return (0);
}

/*
 * wts_put_col --
 *	Replace an element in a column database.
 */
static int
wts_put_col(u_int64_t keyno)
{
	static DBT data;
	DB *db;
	ENV *env;
	WT_TOC *toc;
	int notfound, ret;

	db = g.wts_db;
	toc = g.wts_toc;
	env = db->env;

	data_gen(&data, 0);

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log, "put %llu {%.*s}\n",
		    (unsigned long long)keyno,
		    (int)data.size, (char *)data.data);

	if (bdb_put(keyno, data.data, data.size, &notfound))
		return (1);
	
	if ((ret = db->col_put(
	    db, toc, keyno, &data, 0)) != 0 && ret != WT_NOTFOUND) {
		env->err(env, ret, "wts_put_col: put row %llu by key",
		    (unsigned long long)keyno);
		return (1);
	}
	return (0);
}

/*
 * wts_del_row --
 *	Delete an element from a row database.
 */
static int
wts_del_row(u_int64_t keyno)
{
	static DBT key;
	DB *db;
	ENV *env;
	WT_TOC *toc;
	int notfound, ret;

	db = g.wts_db;
	toc = g.wts_toc;
	env = db->env;

	key_gen(&key, keyno);

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log,
		    "delete %llu\n", (unsigned long long)keyno);

	if (bdb_del(keyno, &notfound))
		return (1);

	if ((ret = db->row_del(db, toc, &key, 0)) != 0 && ret != WT_NOTFOUND) {
		env->err(
		    env, ret, "wts_del_row: delete row %llu by key",
		        (unsigned long long)keyno);
		return (1);
	}
	NTF_CHK(wts_notfound_chk(env, "wts_del_row", ret, notfound, keyno));
	return (0);
}

/*
 * wts_del_col --
 *	Delete an element from a column database.
 */
static int
wts_del_col(u_int64_t keyno)
{
	DB *db;
	ENV *env;
	WT_TOC *toc;
	int notfound, ret;

	db = g.wts_db;
	toc = g.wts_toc;
	env = db->env;

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log,
		    "delete %llu\n", (unsigned long long)keyno);

	if (bdb_del(keyno, &notfound))
		return (1);

	if ((ret = db->col_del(db, toc, keyno, 0)) != 0 && ret != WT_NOTFOUND) {
		env->err(env, ret, "wts_del_col: delete row %llu by key",
		        (unsigned long long)keyno);
		return (1);
	}

	NTF_CHK(wts_notfound_chk(env, "wts_del_col", ret, notfound, keyno));
	return (0);
}

/*
 * wts_notfound_chk --
 *	Compare notfound returns for consistency.
 */
static int
wts_notfound_chk(
    ENV *env, const char *f, int wt_ret, int bdb_notfound, u_int64_t keyno)
{
	/* Check for not found status. */
	if (bdb_notfound) {
		if (wt_ret == WT_NOTFOUND)
			return (2);

		env->errx(env,
		    "%s: row %llu: deleted in Berkeley DB, found in WiredTiger",
		    f, (unsigned long long)keyno);
		return (1);
	}
	if (wt_ret == WT_NOTFOUND) {
		env->errx(env,
		    "%s: row %llu: found in Berkeley DB, deleted in WiredTiger",
		    f, (unsigned long long)keyno);
		return (1);
	}
	return (0);
}
