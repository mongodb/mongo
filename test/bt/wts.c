/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

static int cb_bulk(BTREE *, WT_ITEM **, WT_ITEM **);
static int wts_del_col(u_int64_t);
static int wts_del_row(u_int64_t);
static int wts_notfound_chk(const char *, int, int, u_int64_t);
static int wts_put_col(u_int64_t);
static int wts_put_row(u_int64_t);
static int wts_read_col(u_int64_t);
static int wts_read_row(u_int64_t);
static int wts_sync(void);

int
wts_startup(int logfile)
{
	time_t now;
	BTREE *btree;
	CONNECTION *conn;
	SESSION *session;
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

	/* XXX add cachesize to config */
	if ((ret = wiredtiger_simple_setup(g.progname, "memcheck", &btree)) != 0) {
		fprintf(stderr, "%s: wiredtiger_simple_setup: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	conn = btree->conn;

	/* Open the log file. */
	if (logfile) {
		(void)conn->verbose_set(conn,
		    // WT_VERB_FILEOPS |
		    // WT_VERB_HAZARD |
		    // WT_VERB_MUTEX |
		    // WT_VERB_READ |
		    // WT_VERB_EVICT |
		    0);
		(void)conn->msgfile_set(conn, g.wts_log);
	}

	intl_node_max = (u_int)1 << g.c_intl_node_max;
	intl_node_min = (u_int)1 << g.c_intl_node_min;
	leaf_node_max = (u_int)1 << g.c_leaf_node_max;
	leaf_node_min = (u_int)1 << g.c_leaf_node_min;
	if ((ret = btree->btree_pagesize_set(btree, 0,
	    intl_node_min, intl_node_max, leaf_node_min, leaf_node_max)) != 0) {
		fprintf(stderr, "%s: btree_pagesize_set: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	switch (g.c_file_type) {
	case FIX:
		/*
		 * XXX
		 * Don't go past the WT limit of 20 objects per leaf page.
		 */
		if (20 * g.c_data_min > leaf_node_min)
			g.c_data_min = leaf_node_min / 20;
		if ((ret = btree->column_set(btree, g.c_data_min,
		    NULL, g.c_repeat_comp_pct != 0 ? WT_RLE : 0)) != 0) {
			fprintf(stderr, "%s: column_set: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			return (1);
		}
		break;
	case VAR:
		if ((ret = btree->column_set(btree, 0, NULL, 0)) != 0) {
			fprintf(stderr, "%s: column_set: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			return (1);
		}
		/* FALLTHROUGH */
	case ROW:
		if (g.c_huffman_key && (ret = btree->huffman_set(
		    btree, NULL, 0, WT_ASCII_ENGLISH|WT_HUFFMAN_KEY)) != 0) {
			fprintf(stderr, "%s: huffman_set: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			return (1);
		}
		if (g.c_huffman_data && (ret = btree->huffman_set(
		    btree, NULL, 0, WT_ASCII_ENGLISH|WT_HUFFMAN_DATA)) != 0) {
			fprintf(stderr, "%s: huffman_set: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			return (1);
		}
		break;
	}

	p = fname("wt");
	if ((ret = btree->open(btree, p, 0660, WT_CREATE)) != 0) {
		fprintf(stderr, "%s: btree.open: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	if ((ret = btree->conn->session(btree->conn, 0, &session)) != 0) {
		fprintf(stderr, "%s: conn.session: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	g.wts_btree = btree;
	g.wts_session = session;
	return (0);
}

void
wts_teardown(void)
{
	SESSION *session;
	time_t now;

	session = g.wts_session;

	if (g.wts_log != NULL) {
		fprintf(
		    g.wts_log, "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
		(void)time(&now);
		fprintf(g.wts_log, "WT teardown: %s", ctime(&now));
		fprintf(
		    g.wts_log, "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
	}

	assert(wts_sync() == 0);
	assert(session->close(session, 0) == 0);
	assert(wiredtiger_simple_teardown(g.progname, g.wts_btree) == 0);
}

int
wts_bulk_load(void)
{
	BTREE *btree;
	int ret;

	btree = g.wts_btree;

	if ((ret = btree->bulk_load(btree, track, cb_bulk)) != 0) {
		fprintf(stderr, "%s: bulk_load: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

int
wts_dump(void)
{
	BTREE *btree;
	FILE *fp;
	int ret;
	const char *p;

	btree = g.wts_btree;

	/* Dump the WiredTiger file. */
	track("dump", 0);
	p = fname("wt_dump");
	if ((fp = fopen(p, "w")) == NULL) {
		fprintf(stderr, "%s: fopen: %s\n",
		    g.progname, wiredtiger_strerror(errno));
		return (1);
	}
	if ((ret = btree->dump(btree, fp, track, WT_PRINTABLES)) != 0) {
		fprintf(stderr, "%s: btree.dump: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	(void)fclose(fp);

	track("dump comparison", 0);
	switch (g.c_file_type) {
	case FIX:
	case VAR:
		p = "sh ./s_dumpcmp -c";
		break;
	case ROW:
		p = "sh ./s_dumpcmp";
		break;
	}
	if (system(p) != 0) {
		fprintf(stderr, "%s: dump comparison failed\n", g.progname);
		return (1);
	}

	return (0);
}

static int
wts_sync(void)
{
	BTREE *btree;
	int ret;

	btree = g.wts_btree;

	if ((ret = btree->sync(btree, track, WT_OSWRITE)) != 0) {
		fprintf(stderr, "%s: btree.sync: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

int
wts_verify(void)
{
	BTREE *btree;
	int ret;

	btree = g.wts_btree;

	if ((ret = btree->verify(btree, track, 0)) != 0) {
		fprintf(stderr, "%s: btree.verify: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

/*
 * wts_stats --
 *	Dump the run's statistics.
 */
int
wts_stats(void)
{
	BTREE *btree;
	FILE *fp;
	char *p;
	int ret;

	btree = g.wts_btree;

	track("stat", 0);
	p = fname("stats");
	if ((fp = fopen(p, "w")) == NULL) {
		fprintf(stderr, "%s: fopen: %s\n",
		    g.progname, wiredtiger_strerror(errno));
		return (1);
	}
	if ((ret = btree->conn->stat_print(btree->conn, fp, 0)) != 0) {
		fprintf(stderr, "%s: conn.stat_print: %s\n",
		    g.progname, wiredtiger_strerror(ret));
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
cb_bulk(BTREE *btree, WT_ITEM **keyp, WT_ITEM **datap)
{
	static WT_ITEM key, data;

	btree = NULL;					/* Lint */
	++g.key_cnt;

	if (g.key_cnt > g.c_rows) {
		g.key_cnt = g.c_rows;
		return (1);
	}

	key_gen(&key.data, &key.size, g.key_cnt);
	data_gen(&data.data, &data.size, 1);

	switch (g.c_file_type) {
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
wts_ops(void)
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
			switch (g.c_file_type) {
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
			switch (g.c_file_type) {
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

		switch (g.c_file_type) {
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
			track("read/write ops", cnt);
	}
	return (0);
}

/*
 * wts_read_key_scan --
 *	Read and verify elements in a row-store file.
 */
int
wts_read_row_scan(void)
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
 *	Read and verify a single element in a row-store file.
 */
static int
wts_read_row(u_int64_t keyno)
{
	static WT_ITEM key, data, bdb_data;
	BTREE *btree;
	CONNECTION *conn;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;
	conn = btree->conn;

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log, "read %llu\n", (unsigned long long)keyno);

	/* Retrieve the BDB data item. */
	if (bdb_read(keyno, &bdb_data.data, &bdb_data.size, &notfound))
		return (1);

	/* Retrieve the key/data pair by key. */
	key_gen(&key.data, &key.size, keyno);
	if ((ret = btree->row_get(btree, session, &key, &data, 0)) != 0 &&
	    ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: wts_read_key: read row %llu by key: %s\n",
		    g.progname, (unsigned long long)keyno,
		    wiredtiger_strerror(ret));
		return (1);
	}

	/* Check for not-found status. */
	NTF_CHK(wts_notfound_chk("wts_read_row", ret, notfound, keyno));

	/* Compare the two. */
	if (data.size != bdb_data.size ||
	    memcmp(data.data, bdb_data.data, data.size) != 0) {
		fprintf(stderr,
		    "wts_read_key: read row %llu by key:\n",
		    (unsigned long long)keyno);
		__wt_debug_item("\tbdb", &bdb_data, stderr);
		__wt_debug_item("\twt", &data, stderr);
		return (1);
	}
	return (0);
}

/*
 * wts_read_col_scan --
 *	Read and verify elements in a column-store file.
 */
int
wts_read_col_scan(void)
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
 *	Read and verify a single element in a column-store file.
 */
static int
wts_read_col(u_int64_t keyno)
{
	static WT_ITEM data, bdb_data;
	BTREE *btree;
	CONNECTION *conn;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;
	conn = btree->conn;

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log, "read %llu\n", (unsigned long long)keyno);

	/* Retrieve the BDB data item. */
	if (bdb_read(keyno, &bdb_data.data, &bdb_data.size, &notfound))
		return (1);

	/* Retrieve the key/data pair by record number. */
	if ((ret = btree->col_get(
	    btree, session, keyno, &data, 0)) != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
		    "%s: wts_read_recno: read column %llu by recno: %s\n",
		    g.progname, (unsigned long long)keyno,
		    wiredtiger_strerror(ret));
		return (1);
	}

	/* Check for not-found status. */
	NTF_CHK(wts_notfound_chk("wts_read_col", ret, notfound, keyno));

	/* Compare the two. */
	if (data.size != bdb_data.size ||
	    memcmp(data.data, bdb_data.data, data.size) != 0) {
		fprintf(stderr,
		    "wts_read_recno: read column %llu by recno:\n",
		    (unsigned long long)keyno);
		__wt_debug_item("\tbdb data", &bdb_data, stderr);
		__wt_debug_item("\t wt data", &data, stderr);
		return (1);
	}

	return (0);
}

/*
 * wts_put_row --
 *	Replace an element in a row-store file.
 */
static int
wts_put_row(u_int64_t keyno)
{
	static WT_ITEM key, data;
	BTREE *btree;
	CONNECTION *conn;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;
	conn = btree->conn;

	key_gen(&key.data, &key.size, keyno);
	data_gen(&data.data, &key.size, 0);

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log, "put %llu {%.*s}\n",
		    (unsigned long long)keyno,
		    (int)data.size, (char *)data.data);

	if (bdb_put(keyno, data.data, data.size, &notfound))
		return (1);

	if ((ret = btree->row_put(
	    btree, session, &key, &data, 0)) != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: wts_put_row: put row %llu by key: %s\n",
		    g.progname, (unsigned long long)keyno,
		    wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

/*
 * wts_put_col --
 *	Replace an element in a column-store file.
 */
static int
wts_put_col(u_int64_t keyno)
{
	static WT_ITEM data;
	BTREE *btree;
	CONNECTION *conn;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;
	conn = btree->conn;

	data_gen(&data.data, &data.size, 0);

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log, "put %llu {%.*s}\n",
		    (unsigned long long)keyno,
		    (int)data.size, (char *)data.data);

	if (bdb_put(keyno, data.data, data.size, &notfound))
		return (1);
	
	if ((ret = btree->col_put(
	    btree, session, keyno, &data, 0)) != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: wts_put_col: put col %llu by key: %s\n",
		    g.progname, (unsigned long long)keyno,
		    wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

/*
 * wts_del_row --
 *	Delete an element from a row-store file.
 */
static int
wts_del_row(u_int64_t keyno)
{
	static WT_ITEM key;
	BTREE *btree;
	CONNECTION *conn;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;
	conn = btree->conn;

	key_gen(&key.data, &key.size, keyno);

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log,
		    "delete %llu\n", (unsigned long long)keyno);

	if (bdb_del(keyno, &notfound))
		return (1);

	if ((ret = btree->row_del(btree, session, &key, 0)) != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: wts_del_row: delete row %llu by key: %s\n",
		    g.progname, (unsigned long long)keyno,
		    wiredtiger_strerror(ret));
		return (1);
	}
	NTF_CHK(wts_notfound_chk("wts_del_row", ret, notfound, keyno));
	return (0);
}

/*
 * wts_del_col --
 *	Delete an element from a column-store file.
 */
static int
wts_del_col(u_int64_t keyno)
{
	BTREE *btree;
	CONNECTION *conn;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;
	conn = btree->conn;

	/* Log the operation */
	if (g.wts_log != NULL)
		fprintf(g.wts_log,
		    "delete %llu\n", (unsigned long long)keyno);

	if (bdb_del(keyno, &notfound))
		return (1);

	if ((ret = btree->col_del(btree, session, keyno, 0)) != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: wts_del_col: delete col %llu by key: %s\n",
		    g.progname, (unsigned long long)keyno,
		    wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(wts_notfound_chk("wts_del_col", ret, notfound, keyno));
	return (0);
}

/*
 * wts_notfound_chk --
 *	Compare notfound returns for consistency.
 */
static int
wts_notfound_chk(const char *f, int wt_ret, int bdb_notfound, u_int64_t keyno)
{
	/* Check for not found status. */
	if (bdb_notfound) {
		if (wt_ret == WT_NOTFOUND)
			return (2);

		fprintf(stderr, "%s: %s: row %llu: "
		    "deleted in Berkeley DB, found in WiredTiger\n",
		    g.progname, f, (unsigned long long)keyno);
		return (1);
	}
	if (wt_ret == WT_NOTFOUND) {
		fprintf(stderr, "%s: %s: row %llu: "
		    "found in Berkeley DB, deleted in WiredTiger\n",
		    g.progname, f, (unsigned long long)keyno);
		return (1);
	}
	return (0);
}
