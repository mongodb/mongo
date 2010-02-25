/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

static int cb_bulk(DB *, DBT **, DBT **);

int
wts_setup(int logfile)
{
	ENV *env;
	DB *db;
	int ret;
	char *p;

	if ((ret = wiredtiger_simple_setup(g.progname, &db)) != 0) {
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
		if ((g.logfp = fopen(p, "w")) != NULL) {
			fprintf(stderr,
			    "%s: %s: %s\n", g.progname, p, strerror(errno));
			exit (EXIT_FAILURE);
		}
		env->msgfile_set(env, g.logfp);
	}

	if ((ret = env->cache_size_set(env, (u_int32_t)g.c_cache)) != 0) {
		env->err(env, ret, "Db.column_set");
		return (1);
	}

	if ((ret = db->btree_pagesize_set(
	    db, 0, 1 << g.c_internal_node, 1 << g.c_leaf_node, 0)) != 0) {
		db->err(db, ret, "Db.btree_pagesize_set");
		return (1);
	}

	switch (g.c_database_type) {
	case COLUMN_FIX:
		if ((ret = db->column_set(db, g.c_fixed_length,
		    NULL, g.c_repeat_comp ? WT_REPEAT_COMP : 0)) != 0) {
			db->err(db, ret, "Db.column_set");
			return (1);
		}
		break;
	case COLUMN_VAR:
		if ((ret = db->column_set(db, 0, NULL, 0)) != 0) {
			db->err(db, ret, "Db.column_set");
			return (1);
		}
		break;
	case ROW:
		break;
	}

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

	p = fname(WT_PREFIX, "db");
	(void)remove(p);
	if ((ret = db->open(db, p, 0660, WT_CREATE)) != 0) {
		db->err(db, ret, "Db.open: %s", p);
		return (1);
	}

	g.wts_db = db;
	return (0);
}

void
wts_teardown()
{
	assert(wiredtiger_simple_teardown(g.progname, g.wts_db) == 0);
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
	case COLUMN_FIX:
	case COLUMN_VAR:
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

	if ((ret = db->verify(db, track, 0)) != 0) {
		db->err(db, ret, "Db.verify");
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

	if (g.stats) {
		track("stat", 0);
		p = fname(WT_PREFIX, "load.stat");
		if ((fp = fopen(p, "w")) == NULL) {
			db->err(db, errno, "fopen: %s", p);
			return (1);
		}
		if ((ret = db->env->stat_print(db->env, stdout, 0)) != 0) {
			db->err(db, ret, "Env.stat_print");
			return (1);
		}
		(void)fclose(fp);
	}
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

	if (++g.key_cnt > g.c_bulk_keys) {
		g.key_cnt = g.c_bulk_keys;
		return (1);
	}

	switch (g.c_database_type) {
	case COLUMN_FIX:
	case COLUMN_VAR:
		*keyp = NULL;
		*datap = &data;
		data_gen(&data);
		break;
	case ROW:
		*keyp = &key;
		key_gen(&key, g.key_cnt);
		*datap = &data;
		data_gen(&data);
		break;
	}

	/* Insert the item into BDB. */
	bdb_insert(key.data, key.size, data.data, data.size);

	return (0);
}

/*
 * wts_read_key --
 *	Read random key/data pairs by key.
 */
int
wts_read_key()
{
	DB *db;
	DBT key, data, bdb_data;
	ENV *env;
	WT_TOC *toc;
	u_int64_t cnt, last_cnt;
	int ret;

	db = g.wts_db;
	env = db->env;

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));

	if ((ret = env->toc(env, 0, &toc)) != 0) {
		env->err(env, ret, "Env.toc");
		return (1);
	}

	/* Check a random subset of the records using the key. */
	for (last_cnt = cnt = 0; cnt < g.key_cnt;) {
		cnt += rand() % 17 + 1;
		if (cnt > g.key_cnt)
			cnt = g.key_cnt;
		if (cnt - last_cnt > 1000) {
			track("read key", cnt);
			last_cnt = cnt;
		}

		/* Retrieve the key/data pair by key. */
		key_gen(&key, cnt);
		if ((ret = db->get(db, toc, &key, NULL, &data, 0)) != 0) {
			env->err(env, ret,
			    "wts_read_key: read row %llu by key", cnt);
			return (1);
		}

		/* Retrieve the BDB data item. */
		if (bdb_read_key(
		    key.data, key.size, &bdb_data.data, &bdb_data.size))
			return (1);

		/* Compare the two. */
		if (data.size != bdb_data.size ||
		    memcmp(data.data, bdb_data.data, data.size) != 0) {
			env->errx(env,
			    "wts_read_key: read row %llu by key: expected "
			    "{%.*s}, got {%.*s}",
			    cnt,
			    (int)bdb_data.size, bdb_data.data,
			    (int)data.size, data.data);
			return (1);
		}
	}

	if ((ret = toc->close(toc, 0)) != 0) {
		env->err(env, ret, "Toc.close");
		return (1);
	}
	return (0);
}

/*
 * wts_read_recno --
 *	Read random key/data pairs by record number.
 */
int
wts_read_recno()
{
	DB *db;
	ENV *env;
	DBT key, data, bdb_data, bdb_key;
	WT_TOC *toc;
	u_int64_t cnt, last_cnt;
	int ret;

	db = g.wts_db;
	env = db->env;

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));

	if ((ret = env->toc(env, 0, &toc)) != 0) {
		env->err(env, ret, "Env.toc");
		return (1);
	}

	/* Check a random subset of the records using the record number. */
	for (last_cnt = cnt = 0; cnt < g.key_cnt;) {
		cnt += rand() % 17 + 1;
		if (cnt > g.key_cnt)
			cnt = g.key_cnt;
		if (cnt - last_cnt > 1000) {
			track("read recno", cnt);
			last_cnt = cnt;
		}

		/* Retrieve the key/data pair by record number. */
		key_gen(&key, (u_int)cnt);
		if ((ret = db->get_recno(
		    db, toc, cnt, &key, NULL, &data, 0)) != 0) {
			env->err(env, ret,
			    "wts_read_recno: read row %llu by recno", cnt);
			return (1);
		}

		/* Retrieve the BDB data item. */
		if (bdb_read_recno(cnt, &bdb_key.data,
		    &bdb_key.size, &bdb_data.data, &bdb_data.size))
			return (1);

		/* Compare the two. */
		if (key.size != bdb_key.size ||
		    memcmp(key.data, bdb_key.data, key.size) != 0 ||
		    data.size != bdb_data.size ||
		    memcmp(data.data, bdb_data.data, data.size) != 0) {
			env->errx(env,
			    "wts_read_recno: read row %llu by recno: expected "
			    "{%.*s}/{%.*s}, got {%.*s}/{%.*s}",
			    cnt,
			    (int)bdb_key.size, bdb_key.data,
			    (int)bdb_data.size, bdb_data.data,
			    (int)key.size, key.data,
			    (int)data.size, data.data);
			return (1);
		}
	}

	if ((ret = toc->close(toc, 0)) != 0) {
		env->err(env, ret, "Toc.close");
		return (1);
	}
	return (0);
}
