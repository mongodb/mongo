/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wts.h"

static int bulk(WT_ITEM **, WT_ITEM **);
static int wts_col_del(uint64_t);
static int wts_col_put(uint64_t);
static int wts_notfound_chk(const char *, int, int, uint64_t);
static int wts_read(uint64_t);
static int wts_row_del(uint64_t);
static int wts_row_put(uint64_t, int);
static int wts_sync(void);

static void
handle_error(WT_EVENT_HANDLER *handler, int error, const char *errmsg)
{
	WT_UNUSED(handler);
	WT_UNUSED(error);

	fprintf(stderr, "%s\n", errmsg);
}

static int
handle_message(WT_EVENT_HANDLER *handler, const char *message)
{
	WT_SESSION *session;

	WT_UNUSED(handler);
	session = g.wts_session;

	if (g.logfp != NULL)
		fprintf(g.logfp, "%s\n", message);
	else
		printf("%s\n", message);
	return (0);
}

/*
 * __handle_progress_default --
 *	Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
handle_progress(WT_EVENT_HANDLER *handler,
     const char *operation, uint64_t progress)
{
	WT_UNUSED(handler);

	track(operation, progress);
	return (0);
}


static WT_EVENT_HANDLER event_handler = {
	handle_error,
	handle_message,
	handle_progress
};

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
	    "error_prefix=\"%s\",cache_size=%" PRIu32 "MB,verbose=[%s]",
	    g.progname, g.c_cache,
	    ""
	    // "evict,"
	    // "fileops,"
	    // "hazard,"
	    // "mutex,"
	    // "read,"
	);

	ret = wiredtiger_open(NULL, &event_handler, config, &conn);
	if (ret != 0) {
		fprintf(stderr, "%s: wiredtiger_open: %s\n",
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
	p += snprintf(p, (size_t)(end - p),
	    "key_format=%s,"
	    "intl_node_min=%d,intl_node_max=%d,"
	    "leaf_node_min=%d,leaf_node_max=%d,"
	    "btree_split_min",
	    (g.c_file_type == ROW) ? "u" : "r",
	    intl_node_min = 1U << g.c_intl_node_min,
	    intl_node_max = 1U << g.c_intl_node_max,
	    leaf_node_min = 1U << g.c_leaf_node_min,
	    leaf_node_max = 1U << g.c_leaf_node_max);

	switch (g.c_file_type) {
	case FIX:
		/*
		 * XXX
		 * Don't go past the WT limit of 20 objects per leaf page.
		 */
		if (20 * g.c_data_min > leaf_node_min)
			g.c_data_min = leaf_node_min / 20;
		p += snprintf(p,
		    (size_t)(end - p), ",value_format=\"%du\"", g.c_data_min);
		if (g.c_repeat_comp_pct != 0)
			p += snprintf(
			    p, (size_t)(end - p), ",runlength_encoding");
		break;
	case ROW:
		if (g.c_huffman_key)
			p += snprintf(
			    p, (size_t)(end - p), ",huffman_key=english");
		/* FALLTHROUGH */
	case VAR:
		if (g.c_huffman_value)
			p += snprintf(
			    p, (size_t)(end - p), ",huffman_value=english");
		break;
	}

	WT_ASSERT((WT_SESSION_IMPL *)session, p < end);

	if ((ret = session->create(session, WT_TABLENAME, config)) != 0) {
		fprintf(stderr, "%s: create table: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	if ((ret = session->open_cursor(session, WT_TABLENAME,
	    NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: open_cursor: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	if (g.logging) {
		time(&now);
		session->msg_printf(session,
		    "===============\nWT start: %s===============",
		    ctime(&now));
	}

	g.wts_conn = conn;
	g.wts_session = session;
	g.wts_cursor = cursor;
	return (0);
}

void
wts_teardown(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	time_t now;

	conn = g.wts_conn;
	session = g.wts_session;

	if (g.logging) {
		time(&now);
		session->msg_printf(session,
		    "===============\nWT stop: %s===============",
		    ctime(&now));
	}

	assert(wts_sync() == 0);
	g.wts_session = NULL;
	assert(conn->close(conn, NULL) == 0);
}

int
wts_bulk_load(void)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	WT_ITEM *key, *value;
	uint64_t insert_count;
	int ret;

	session = g.wts_session;

	if ((ret = session->open_cursor(
	    session, WT_TABLENAME, NULL, "bulk", &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	insert_count = 0;
	while (bulk(&key, &value) == 0) {
                /* Report on progress every 100 inserts. */
                if (++insert_count % 100 == 0)
                        track("bulk load", insert_count);
	
		if (key != NULL)
			cursor->set_key(cursor, key);
		cursor->set_value(cursor, value);
		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr, "%s: cursor insert failed: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			ret = 1;
			goto err;
		}
	}

err:	(void)cursor->close(cursor, NULL);
	return (ret);
}

int
wts_dump(void)
{
	WT_CURSOR *cursor;
	WT_ITEM key, value;
	WT_SESSION *session;
	FILE *fp;
	int ret;
	uint64_t row_count;
	const char *p;

	/* Dump the WiredTiger file. */
	session = g.wts_session;
	if ((ret = session->open_cursor(session, WT_TABLENAME, NULL,
	    "dump,printable", &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	track("dump", 0);
	p = fname("wt_dump");
	if ((fp = fopen(p, "w")) == NULL) {
		fprintf(stderr, "%s: fopen: %s\n",
		    g.progname, wiredtiger_strerror(errno));
		return (1);
	}

        fprintf(fp, "VERSION=1\n");
        fprintf(fp, "HEADER=END\n");
	row_count = 0;
	while ((ret = cursor->next(cursor)) == 0) {
		if (++row_count % 100 == 0)
			track("dump", row_count);
		if (cursor->get_key(cursor, &key) == 0 && key.data != NULL) {
			fwrite(key.data, key.size, 1, fp);
			fwrite("\n", 1, 1, fp);
		}
		cursor->get_value(cursor, &value);
		fwrite(value.data, value.size, 1, fp);
		fwrite("\n", 1, 1, fp);
	}
	fprintf(fp, "DATA=END\n");
	(void)fclose(fp);
	WT_TRET(cursor->close(cursor, NULL));

	if (ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: dump failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

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
	WT_SESSION *session;
	int ret;

	session = g.wts_session;

	if ((ret = session->salvage(session, WT_TABLENAME, NULL)) != 0) {
		fprintf(stderr, "%s: salvage: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}


static int
wts_sync(void)
{
	WT_SESSION *session;
	int ret;

	session = g.wts_session;

	if ((ret = session->sync(session, WT_TABLENAME, NULL)) != 0) {
		fprintf(stderr, "%s: sync: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

int
wts_verify(void)
{
	WT_SESSION *session;
	int ret;

	session = g.wts_session;

	if ((ret = session->verify(session, WT_TABLENAME, NULL)) != 0) {
		fprintf(stderr, "%s: verify: %s\n",
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
	WT_CURSOR *cursor;
	WT_ITEM key, value;
	WT_SESSION *session;
	FILE *fp;
	char *p;
	int ret;

	session = g.wts_session;

	track("stat", 0);
	p = fname("stats");
	if ((fp = fopen(p, "w")) == NULL) {
		fprintf(stderr, "%s: fopen: %s\n",
		    g.progname, wiredtiger_strerror(errno));
		return (1);
	}
	
	if ((ret = session->open_cursor(session, "stat:" WT_TABLENAME, NULL,
	    "printable", &cursor)) != 0) {
		fprintf(stderr, "%s: stat cursor open failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	while ((ret = cursor->next(cursor)) == 0) {
		cursor->get_key(cursor, &key);
		fwrite(key.data, key.size, 1, stdout);
		fwrite("\n", 1, 1, stdout);
		cursor->get_value(cursor, &value);
		fwrite(value.data, value.size, 1, stdout);
		fwrite("\n", 1, 1, stdout);
	}
	(void)fclose(fp);
	(void)cursor->close(cursor, NULL);

	if (ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: stat cursor next: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	return (0);
}

/*
 * bulk --
 *	WiredTiger bulk load routine. 
 */
static int
bulk(WT_ITEM **keyp, WT_ITEM **valuep)
{
	static WT_ITEM key, value;
	WT_SESSION *session;

	session = g.wts_session;
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
			session->msg_printf(session,
                            "%-10s %" PRIu32 " {%.*s}", "bulk K",
                            g.key_cnt, (int)key.size, (char *)key.data);
		break;
	}
	*valuep = &value;
	if (g.logging)
		session->msg_printf(session,
                    "%-10s %" PRIu32 " {%.*s}", "bulk V",
                    g.key_cnt, (int)value.size, (char *)value.data);

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
				if (wts_row_del(keyno))
					return (1);
				break;
			case FIX:
			case VAR:
				if (wts_col_del(keyno))
					return (1);
				break;
			}
		} else if (g.c_file_type == ROW &&
		    op < g.c_delete_pct + g.c_insert_pct) {
			if (wts_row_put(keyno, 1))
				return (1);
		} else if (
		    op < g.c_delete_pct + g.c_insert_pct + g.c_write_pct) {
			switch (g.c_file_type) {
			case ROW:
				if (wts_row_put(keyno, 0))
					return (1);
				break;
			case FIX:
			case VAR:
				if (wts_col_put(keyno))
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
 * wts_read --
 *	Read and verify a single element in a row-store file.
 */
static int
wts_read(uint64_t keyno)
{
	static WT_ITEM key, value, bdb_value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	/* Log the operation */
	if (g.logging)
		session->msg_printf(session, "%-10s%" PRIu64, "read", keyno);

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
		fprintf(stderr, "%s: wts_read: read row %" PRIu64 ": %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	/* Check for not-found status. */
	NTF_CHK(wts_notfound_chk("wts_read", ret, notfound, keyno));

	/* Compare the two. */
	if (value.size != bdb_value.size ||
	    memcmp(value.data, bdb_value.data, value.size) != 0) {
		fprintf(stderr, "wts_read: read row %" PRIu64 ":\n", keyno);
		__wt_debug_pair(
		    "bdb", bdb_value.data, bdb_value.size, stderr);
		__wt_debug_pair("wt", value.data, value.size, stderr);
		return (1);
	}
	return (0);
}

/*
 * wts_row_put --
 *	Replace an element in a row-store file.
 */
static int
wts_row_put(uint64_t keyno, int insert)
{
	static WT_ITEM key, value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, insert);
	value_gen(&value.data, &value.size, 0);

	/* Log the operation */
	if (g.logging)
		session->msg_printf(session, "%-10s{%.*s}\n%-10s{%.*s}",
		    "put key", (int)key.size, (char *)key.data,
		    "put data", (int)value.size, (char *)value.data);

	if (bdb_put(key.data, key.size, value.data, value.size, &notfound))
		return (1);

	cursor->set_key(cursor, &key);
	cursor->set_value(cursor, &value);
	if ((ret = cursor->update(cursor)) != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
                    "%s: wts_row_put: put row %" PRIu64 " by key: %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

/*
 * wts_col_put --
 *	Replace an element in a column-store file.
 */
static int
wts_col_put(uint64_t keyno)
{
	static WT_ITEM key, value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	value_gen(&value.data, &value.size, 0);

	/* Log the operation */
	if (g.logging)
		session->msg_printf(session, "%-10s%" PRIu64 " {%.*s}",
		    "put", keyno, (int)value.size, (char *)value.data);

	key_gen(&key.data, &key.size, keyno, 0);
	if (bdb_put(key.data, key.size, value.data, value.size, &notfound))
		return (1);
	
	cursor->set_key(cursor, (wiredtiger_recno_t)keyno);
	cursor->set_value(cursor, &value);
	if ((ret = cursor->update(cursor)) != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
                    "%s: wts_col_put: put col %" PRIu64 " by key: %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

/*
 * wts_row_del --
 *	Delete an element from a row-store file.
 */
static int
wts_row_del(uint64_t keyno)
{
	static WT_ITEM key;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, 0);

	/* Log the operation */
	if (g.logging)
		session->msg_printf(session, "%-10s%" PRIu64, "delete", keyno);

	if (bdb_del(keyno, &notfound))
		return (1);

	cursor->set_key(cursor, &key);
	if ((ret = cursor->remove(cursor)) != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
                    "%s: wts_row_del: remove %" PRIu64 " by key: %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}
	NTF_CHK(wts_notfound_chk("wts_row_del", ret, notfound, keyno));
	return (0);
}

/*
 * wts_col_del --
 *	Delete an element from a column-store file.
 */
static int
wts_col_del(uint64_t keyno)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	/* Log the operation */
	if (g.logging)
		session->msg_printf(session, "%-10s%" PRIu64, "delete", keyno);

	if (bdb_del(keyno, &notfound))
		return (1);

	cursor->set_key(cursor, keyno);
	if ((ret = cursor->remove(cursor)) != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
                    "%s: wts_col_del: remove %" PRIu64 " by key: %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(wts_notfound_chk("wts_col_del", ret, notfound, keyno));
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

		fprintf(stderr, "%s: %s: row %" PRIu64
                    ": deleted in Berkeley DB, found in WiredTiger\n",
		    g.progname, f, keyno);
		return (1);
	}
	if (wt_ret == WT_NOTFOUND) {
		fprintf(stderr, "%s: %s: row %" PRIu64
		    ": found in Berkeley DB, deleted in WiredTiger\n",
		    g.progname, f, keyno);
		return (1);
	}
	return (0);
}
