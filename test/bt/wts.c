/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

extern void __wt_bt_debug_dbt(const char *, DBT *, FILE *);

static int cb_bulk(DB *, DBT **, DBT **);

int
wts_setup(int reopen, int logfile)
{
	ENV *env;
	DB *db;
	WT_TOC *toc;
	u_int32_t intl_size, leaf_size;
	int ret;
	char *p;

	if ((ret = wiredtiger_simple_setup(
	    g.progname, &db, g.c_cache, WT_MEMORY_CHECK)) != 0) {
		fprintf(stderr, "%s: wiredtiger_simple_setup: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	env = db->env;

	env->errpfx_set(env, g.progname);
	env->errfile_set(env, stderr);

	/* Open the log file. */
	if (logfile) {
		p = fname(WT_PREFIX, "log");
		if ((g.logfp = fopen(p, "w")) == NULL) {
			fprintf(stderr,
			    "%s: %s: %s\n", g.progname, p, strerror(errno));
			exit (EXIT_FAILURE);
		}
		env->verbose_set(env, WT_VERB_CACHE | WT_VERB_SERVERS);
		env->msgfile_set(env, g.logfp);
	}

	intl_size = 1 << g.c_internal_node;
	leaf_size = 1 << g.c_leaf_node;
	if ((ret =
	    db->btree_pagesize_set(db, 0, intl_size, leaf_size, 0)) != 0) {
		db->err(db, ret, "Db.btree_pagesize_set");
		return (1);
	}

	switch (g.c_database_type) {
	case FIX:
		/*
		 * XXX
		 * Don't go past the WT limit of 20 objects per leaf page.
		 */
		if (20 * g.c_data_min > leaf_size)
			g.c_data_min = leaf_size / 20;
		if ((ret = db->column_set(db, g.c_data_min,
		    NULL, g.c_repeat_comp ? WT_REPEAT_COMP : 0)) != 0) {
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

	p = fname(WT_PREFIX, "db");

	if (!reopen)
		(void)remove(p);
	if ((ret = db->open(db, p, 0660, reopen ? 0 : WT_CREATE)) != 0) {
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

	toc = g.wts_toc;

	assert(toc->close(toc, 0) == 0);
	assert(wiredtiger_simple_teardown(g.progname, g.wts_db) == 0);

	if (g.logfp != NULL)
		(void)fclose(g.logfp);
}

int
wts_bulk_load()
{
	DB *db;
	FILE *fp;
	char *p;
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

	if (g.dump) {
		track("dump", 0);
		p = fname(WT_PREFIX, "dump");
		if ((fp = fopen(p, "w")) == NULL) {
			db->err(db, errno, "fopen: %s", p);
			return (1);
		}
		if ((ret = db->dump(db, fp, track,
		    g.dump == DUMP_DEBUG ? WT_DEBUG : WT_PRINTABLES)) != 0) {
			db->err(db, ret, "Db.dump");
			return (1);
		}
		(void)fclose(fp);
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

	track("stat", 0);
	p = fname(NULL, "stats");
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

	db = NULL;

	/*
	 * Generate a set of duplicates for each key if duplicates have been
	 * configured.  The duplicate_pct configuration is a percentage, which
	 * defines the number of keys that get duplicate data items, and the
	 * number of duplicate data items for each such key is a random value
	 * in-between 2 and the value of duplicate_cnt.
	 */
	if (g.c_duplicates_pct == 0 ||
	    (u_int32_t)rand() % 100 > g.c_duplicates_pct) {
		if (++g.key_cnt > g.c_total) {
			g.key_cnt = g.c_total;
			return (1);
		}

		key_gen(&key, g.key_cnt);
	}
	data_gen(&data);

	switch (g.c_database_type) {
	case FIX:
	case VAR:
		*keyp = NULL;
		*datap = &data;
		break;
	case ROW:
		*keyp = &key;
		*datap = &data;
		break;
	}

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

	for (cnt = 0; cnt < g.c_ops; ++cnt) {
		/*
		 * Perform some number of read/write operations.  Deletes are
		 * not separately configured, they're a fixed percent of write
		 * operations.
		 */
		keyno = MMRAND(1, g.c_total);
		if ((u_int32_t)rand() % 100 <= g.c_read_pct) {
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
		} else if (rand() % 100 <= 5) {
			if (wts_del(keyno))
				return (1);
		}

		if (cnt % 1000 == 0)
			track("read/write ops", cnt);
	}
	return (0);
}

/*
 * wts_read_key_scan --
 *	Read some number of row database entries by record key.
 */
int
wts_read_row_scan()
{
	u_int64_t cnt, last_cnt;

	/* Check a random subset of the records using the key. */
	for (last_cnt = cnt = 0; cnt < g.key_cnt;) {
		cnt += rand() % 17 + 1;
		if (cnt > g.c_total)
			cnt = g.c_total;
		if (cnt - last_cnt > 1000) {
			track("read row scan", cnt);
			last_cnt = cnt;
		}

		if (wts_read_row(cnt))
			return (1);
	}
	return (0);
}

/*
 * wts_read_row --
 *	Read and verify a single row database record by key.
 */
int
wts_read_row(u_int64_t keyno)
{
	static DBT key, data, bdb_data;
	DB *db;
	ENV *env;
	WT_TOC *toc;
	int ret, notfound;

	db = g.wts_db;
	toc = g.wts_toc;
	env = db->env;

	key_gen(&key, keyno);

	/* Retrieve the BDB data item. */
	if (bdb_read_key(
	    key.data, key.size, &bdb_data.data, &bdb_data.size, &notfound))
		return (1);

	/* Retrieve the key/data pair by key. */
	if ((ret = db->get(
	    db, toc, &key, NULL, &data, 0)) != 0 && ret != WT_NOTFOUND) {
		env->err(env, ret,
		    "wts_read_key: read row %llu by key", keyno);
		return (1);
	}

	/* Check for not found status. */
	if (notfound) {
		if (ret == WT_NOTFOUND)
			return (0);
		env->errx(env, "WT returned deleted row %llu", keyno);
		return (1);
	}
	if (ret != 0) {
		env->errx(env,
		    "WT returned existing row %llu as deleted", keyno);
		return (1);
	} 

	/* Compare the two. */
	if (data.size != bdb_data.size ||
	    memcmp(data.data, bdb_data.data, data.size) != 0) {
		fprintf(stderr,
		    "wts_read_key: read row %llu by key:\n", keyno);
		__wt_bt_debug_dbt("\tbdb", &bdb_data, stderr);
		__wt_bt_debug_dbt("\twt", &data, stderr);
		return (1);
	}
	return (0);
}

/*
 * wts_read_col_scan --
 *	Read some number of column database entries by record number.
 */
int
wts_read_col_scan()
{
	u_int64_t cnt, last_cnt;

	/* Check a random subset of the records using the record number. */
	for (last_cnt = cnt = 0; cnt < g.c_total;) {
		cnt += rand() % 17 + 1;
		if (cnt > g.c_total)
			cnt = g.c_total;
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
 *	Read and verify a single record by record number.
 */
int
wts_read_col(u_int64_t keyno)
{
	static DBT data, bdb_data;
	DB *db;
	ENV *env;
	WT_TOC *toc;
	int ret;

	db = g.wts_db;
	toc = g.wts_toc;
	env = db->env;

	/* Retrieve the key/data pair by record number. */
	if ((ret = db->get_recno(db, toc, keyno, &data, 0)) != 0) {
		env->err(env, ret,
		    "wts_read_recno: read column %llu by recno", keyno);
		return (1);
	}

	/* Retrieve the BDB data item. */
	if (bdb_read_recno(keyno, &bdb_data.data, &bdb_data.size))
		return (1);

	/* Compare the two. */
	if (data.size != bdb_data.size ||
	    memcmp(data.data, bdb_data.data, data.size) != 0) {
		fprintf(stderr,
		    "wts_read_recno: read column %llu by recno:\n", keyno);
		__wt_bt_debug_dbt("\tbdb data", &bdb_data, stderr);
		__wt_bt_debug_dbt("\t wt data", &data, stderr);
		return (1);
	}

	return (0);
}

/*
 * wts_del --
 *	Delete a record by key.
 */
int
wts_del(u_int64_t keyno)
{
	static DBT key;
	DB *db;
	ENV *env;
	WT_TOC *toc;
	int notfound, ret;

	db = g.wts_db;
	toc = g.wts_toc;
	env = db->env;

	/*
	 * Delete the key/data pair by key -- if the item has already been
	 * deleted, then we'll get back WT_NOTFOUND.
	 */
	key_gen(&key, keyno);
	if ((ret = db->del(db, toc, &key, 0)) != 0 && ret != WT_NOTFOUND) {
		env->err(env, ret, "wts_del: delete row %llu by key", keyno);
		return (1);
	}
	if (bdb_del(key.data, key.size, &notfound))
		return (1);
	if (ret == WT_NOTFOUND) {
		if (notfound)
			return (0);
		env->errx(env,
		    "wts_del: delete row %llu by key returned WT_NOTFOUND for "
		    "existing key", keyno);
		return (1);
	}
	return (0);
}
