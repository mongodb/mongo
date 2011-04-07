/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wts.h"

static int cb_bulk(BTREE *, WT_ITEM **, WT_ITEM **);
static int wts_del_col(u_int64_t);
static int wts_del_row(u_int64_t);
static int wts_notfound_chk(const char *, int, int, u_int64_t);
static int wts_put_col(u_int64_t);
static int wts_put_row(u_int64_t, int);
static int wts_read(uint64_t);
static int wts_sync(void);

int
wts_startup(void)
{
	time_t now;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint32_t intl_node_max, intl_node_min, leaf_node_max, leaf_node_min;
	int ret;
	char config[200], *end, *p;

	snprintf(config, sizeof(config),
	    "error_prefix=\"%s\",cache_size=%d,%sverbose=[%s]",
	    g.progname, g.c_cache,
	    g.logging ? ",logging" : "",
	    ""
	    // "fileops,"
	    // "hazard,"
	    // "mutex,"
	    // "read,"
	    // "evict,"
	);

	if ((ret = wiredtiger_open(NULL, NULL, config, &conn)) != 0) {
		fprintf(stderr, "%s: wiredtiger_simple_setup: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	if ((ret = conn->open_session(conn, NULL, NULL,
	    &session)) != 0) {
		fprintf(stderr, "%s: conn.session: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	p = config;
	end = config + sizeof(config);
	p += snprintf(p, end - p, "key_format=%s,"
	    "intl_node_min=%d,intl_node_max=%d,"
	    "leaf_node_min=%d,leaf_node_max=%d",
	    (g.c_file_type == ROW) ? "u" : "r",
	    intl_node_min = 1 << g.c_intl_node_min,
	    intl_node_max = 1 << g.c_intl_node_max,
	    leaf_node_min = 1 << g.c_leaf_node_min,
	    leaf_node_max = 1 << g.c_leaf_node_max);

	switch (g.c_file_type) {
	case FIX:
		/*
		 * XXX
		 * Don't go past the WT limit of 20 objects per leaf page.
		 */
		if (20 * g.c_data_min > leaf_node_min)
			g.c_data_min = leaf_node_min / 20;
		p += snprintf(p, end - p,
		    ",value_format=\"%du\"", g.c_data_min);
		if (g.c_repeat_comp_pct != 0)
			p += snprintf(p, end - p, ",runlength_encoding");
		break;
	case VAR:
	case ROW:
		if (g.c_huffman_key)
			p += snprintf(p, end - p, ",huffman_key=english");
		if (g.c_huffman_data)
			p += snprintf(p, end - p, ",huffman_value=english");
		break;
	}

	WT_ASSERT((SESSION *)session, p < end);

	if ((ret = session->create_table(session, "__wt.wt", config)) != 0) {
		fprintf(stderr, "%s: session.create_table: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	if ((ret = session->open_cursor(session, "table:__wt.wt",
	    NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: session.open_cursor: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	if (g.logging)
		__wt_log_printf((SESSION *)session, "WT startup: %s",
		    ctime(&now));

	g.wts_conn = conn;
	g.wts_session = session;
	g.wts_cursor = cursor;
	g.wts_btree = ((CURSOR_BTREE *)cursor)->btree;
	return (0);
}

void
wts_teardown(void)
{
	WT_CONNECTION *conn;
	SESSION *session;
	time_t now;

	conn = g.wts_conn;
	session = g.wts_session;

	if (g.logging)
		__wt_log_printf(session, "WT teardown: %s",
		    ctime(&now));

	assert(wts_sync() == 0);
	assert(conn->close(conn, NULL) == 0);
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

int
wts_salvage(void)
{
	BTREE *btree;
	SESSION *session;
	int ret;
	char *p;

	btree = g.wts_btree;
	session = g.wts_session;

	p = fname("wt");
	if ((ret = btree->salvage(btree, session, track, 0)) != 0) {
		fprintf(stderr, "%s: btree.salvage: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}


static int
wts_sync(void)
{
	BTREE *btree;
	SESSION *session;
	int ret;

	btree = g.wts_btree;
	session = g.wts_session;

	if ((ret = btree->sync(btree, session, track, WT_OSWRITE)) != 0) {
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
	SESSION *session;
	int ret;

	btree = g.wts_btree;
	session = g.wts_session;

	if ((ret = btree->verify(btree, session, track, 0)) != 0) {
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
	CONNECTION *conn;
	BTREE *btree;
	FILE *fp;
	char *p;
	int ret;

	btree = g.wts_btree;
	conn = btree->conn;

	track("stat", 0);
	p = fname("stats");
	if ((fp = fopen(p, "w")) == NULL) {
		fprintf(stderr, "%s: fopen: %s\n",
		    g.progname, wiredtiger_strerror(errno));
		return (1);
	}
	if ((ret = conn->stat_print(conn, fp, 0)) != 0) {
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
cb_bulk(BTREE *btree, WT_ITEM **keyp, WT_ITEM **valuep)
{
	static WT_ITEM key, value;

	btree = NULL;					/* Lint */
	++g.key_cnt;

	if (g.key_cnt > g.c_rows) {
		g.key_cnt = g.c_rows;
		return (1);
	}

	key_gen(&key.data, &key.size, g.key_cnt, 0);
	value_gen(&value.data, &value.size, 1);

	switch (g.c_file_type) {
	case FIX:
	case VAR:
		*keyp = NULL;
		break;
	case ROW:
		*keyp = &key;
		if (g.logging)
			__wt_log_printf(g.wts_session, "%-10s{%.*s}",
			    "bulk key", (int)key.size, (char *)key.data);
		break;
	}
	*valuep = &value;
	if (g.logging)
		__wt_log_printf(g.wts_session, "%-10s{%.*s}",
		    "bulk value", (int)value.size, (char *)value.data);

	/* Insert the item into BDB. */
	bdb_insert(key.data, key.size, value.data, value.size);

	return (0);
}

/*
 * wts_ops --
 *	Perform a number of operations.
 */
int
wts_ops(void)
{
	uint64_t keyno;
	u_int cnt;
	uint32_t op;

	for (cnt = 0; cnt < g.c_ops; ++cnt) {
		keyno = MMRAND(1, g.c_rows);

		/*
		 * Perform some number of operations: the percentage of deletes,
		 * inserts and writes are specified, reads are the rest.  The
		 * percentages don't have to add up to 100, a high percentage
		 * of deletes will mean fewer inserts and writes.  A read
		 * operation always follows a modification to confirm it worked.
		 */
		op = (uint32_t)(wts_rand() % 100);
		if (op < g.c_delete_pct) {
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
		} else if (g.c_file_type == ROW &&
		    op < g.c_delete_pct + g.c_insert_pct) {
			if (wts_put_row(keyno, 1))
				return (1);
		} else if (
		    op < g.c_delete_pct + g.c_insert_pct + g.c_write_pct) {
			switch (g.c_file_type) {
			case ROW:
				if (wts_put_row(keyno, 0))
					return (1);
				break;
			case FIX:
			case VAR:
				if (wts_put_col(keyno))
					return (1);
				break;
			}
		}

		if (wts_read(keyno))
			return (1);

		if (cnt % 10 == 0)
			track("read/write ops", cnt);
	}
	return (0);
}

/*
 * wts_read_scan --
 *	Read and verify all elements in a file.
 */
int
wts_read_scan(void)
{
	uint64_t cnt, last_cnt;

	/* Check a random subset of the records using the key. */
	for (last_cnt = cnt = 0; cnt < g.key_cnt;) {
		cnt += wts_rand() % 17 + 1;
		if (cnt > g.c_rows)
			cnt = g.c_rows;
		if (cnt - last_cnt > 1000) {
			track("read row scan", cnt);
			last_cnt = cnt;
		}

		if (wts_read(cnt))
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
wts_read(uint64_t keyno)
{
	static WT_ITEM key, value, bdb_value;
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;
	int notfound, ret;

	btree = g.wts_cursor;
	cursor = g.wts_cursor;
	session = g.wts_session;

	/* Log the operation */
	if (g.logging)
		__wt_log_printf(session, "%-10s%llu", "read",
		    (unsigned long long)keyno);

	/* Retrieve the BDB value. */
	if (bdb_read(keyno, &bdb_value.data, &bdb_value.size, &notfound))
		return (1);

	/* Retrieve the key/value pair by key. */
	switch (g.c_file_type) {
	case ROW:
		key_gen(&key.data, &key.size, keyno, 0);
		cursor->set_key(cursor, &key);
		break;
	case FIX:
	case VAR:
		cursor->set_key(cursor, keyno);
		break;
	}

	if ((ret = cursor->search(cursor)) == 0)
		ret = cursor->get_value(cursor, &value);
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: wts_read: read row %llu: %s\n",
		    g.progname, (unsigned long long)keyno,
		    wiredtiger_strerror(ret));
		return (1);
	}

	/* Check for not-found status. */
	NTF_CHK(wts_notfound_chk("wts_read_row", ret, notfound, keyno));

	/* Compare the two. */
	if (value.size != bdb_value.size ||
	    memcmp(value.data, bdb_value.data, value.size) != 0) {
		fprintf(stderr,
		    "wts_read: read row %llu:\n",
		    (unsigned long long)keyno);
		__wt_debug_item("\tbdb", &bdb_value, stderr);
		__wt_debug_item("\twt", &value, stderr);
		return (1);
	}
	return (0);
}

/*
 * wts_put_row --
 *	Replace an element in a row-store file.
 */
static int
wts_put_row(uint64_t keyno, int insert)
{
	static WT_ITEM key, value;
	BTREE *btree;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, insert);
	value_gen(&value.data, &value.size, 0);

	/* Log the operation */
	if (g.logging)
		__wt_log_printf(session, "%-10s{%.*s}\n%-10s{%.*s}",
		    "put key", (int)key.size, (char *)key.data,
		    "put data", (int)value.size, (char *)value.data);

	if (bdb_put(key.data, key.size, value.data, value.size, &notfound))
		return (1);

	if ((ret = btree->row_put(
	    btree, session, &key, &value, 0)) != 0 && ret != WT_NOTFOUND) {
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
wts_put_col(uint64_t keyno)
{
	static WT_ITEM key, value;
	BTREE *btree;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;

	value_gen(&value.data, &value.size, 0);

	/* Log the operation */
	if (g.logging)
		__wt_log_printf(session, "%-10s%llu {%.*s}",
		    "put", (unsigned long long)keyno,
		    (int)value.size, (char *)value.data);

	key_gen(&key.data, &key.size, keyno, 0);
	if (bdb_put(key.data, key.size, value.data, value.size, &notfound))
		return (1);
	
	if ((ret = btree->col_put(
	    btree, session, keyno, &value, 0)) != 0 && ret != WT_NOTFOUND) {
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
wts_del_row(uint64_t keyno)
{
	static WT_ITEM key;
	BTREE *btree;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, 0);

	/* Log the operation */
	if (g.logging)
		__wt_log_printf(session, "%-10s%llu",
		    "delete", (unsigned long long)keyno);

	if (bdb_del(keyno, &notfound))
		return (1);

	if ((ret = btree->row_del(btree, session, &key, 0)) != 0 &&
	    ret != WT_NOTFOUND) {
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
wts_del_col(uint64_t keyno)
{
	BTREE *btree;
	SESSION *session;
	int notfound, ret;

	btree = g.wts_btree;
	session = g.wts_session;

	/* Log the operation */
	if (g.logging)
		__wt_log_printf(session, "%-10s%llu",
		    "delete", (unsigned long long)keyno);

	if (bdb_del(keyno, &notfound))
		return (1);

	if ((ret = btree->col_del(btree, session, keyno, 0)) != 0 &&
	    ret != WT_NOTFOUND) {
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
wts_notfound_chk(const char *f, int wt_ret, int bdb_notfound, uint64_t keyno)
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
