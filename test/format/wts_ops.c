/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "format.h"

static int  col_del(uint64_t, int *);
static int  col_insert(uint64_t *);
static int  col_put(uint64_t);
static int  nextprev(int, int, int *);
static int  notfound_chk(const char *, int, int, uint64_t);
static int  read_row(uint64_t);
static int  row_del(uint64_t, int *);
static int  row_put(uint64_t, int);
static void print_item(const char *, WT_ITEM *);

/*
 * wts_ops --
 *	Perform a number of operations.
 */
int
wts_ops(void)
{
	uint64_t cnt, keyno;
	uint32_t op;
	u_int np;
	int dir, insert, notfound;

	for (cnt = 0; cnt < g.c_ops; ++cnt) {
		if (cnt % 10 == 0)
			track("read/write ops", cnt);

		insert = notfound = 0;
		keyno = MMRAND(1, g.rows);

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
				/*
				 * If deleting a non-existent record, the cursor
				 * won't be positioned, and so can't do a next.
				 */
				if (row_del(keyno, &notfound))
					return (1);
				break;
			case FIX:
			case VAR:
				if (col_del(keyno, &notfound))
					return (1);
				break;
			}
		} else if (op < g.c_delete_pct + g.c_insert_pct) {
			switch (g.c_file_type) {
			case ROW:
				if (row_put(keyno, 1))
					return (1);
				break;
			case FIX:
			case VAR:
				if (col_insert(&keyno))
					return (1);
				insert = 1;
				break;
			}
		} else if (
		    op < g.c_delete_pct + g.c_insert_pct + g.c_write_pct) {
			switch (g.c_file_type) {
			case ROW:
				if (row_put(keyno, 0))
					return (1);
				break;
			case FIX:
			case VAR:
				if (col_put(keyno))
					return (1);
				break;
			}
		} else {
			if (read_row(keyno))
				return (1);
			continue;
		}

		/*
		 * If we did any operation, we've set the cursor, do a small
		 * number of next/prev cursor operations in a random direction.
		 */
		dir = MMRAND(0, 1);
		for (np = 0; np < MMRAND(1, 8); ++np) {
			if (notfound)
				break;
			if (nextprev(dir, insert, &notfound))
				return (1);
		}

		if (insert) {
			WT_CURSOR *cursor = g.wts_cursor_insert;
			cursor->reset(cursor);
		}

		/* Then read the value we modified to confirm it worked. */
		if (read_row(keyno))
			return (1);
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
		if (cnt > g.rows)
			cnt = g.rows;
		if (cnt - last_cnt > 1000) {
			track("read row scan", cnt);
			last_cnt = cnt;
		}

		if (read_row(cnt))
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
 * read_row --
 *	Read and verify a single element in a row- or column-store file.
 */
static int
read_row(uint64_t keyno)
{
	static WT_ITEM key, value, bdb_value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;
	uint8_t bitfield;

	cursor = g.wts_cursor;
	session = g.wts_session;

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)session->msg_printf(
		    session, "%-10s%" PRIu64, "read", keyno);

	/* Retrieve the BDB value. */
	if (bdb_read(keyno, &bdb_value.data, &bdb_value.size, &notfound))
		return (1);

	/* Retrieve the key/value pair by key. */
	switch (g.c_file_type) {
	case FIX:
	case VAR:
		cursor->set_key(cursor, keyno);
		break;
	case ROW:
		key_gen(&key.data, &key.size, keyno, 0);
		cursor->set_key(cursor, &key);
		break;
	}

	if ((ret = cursor->search(cursor)) == 0) {
		if (g.c_file_type == FIX) {
			ret = cursor->get_value(cursor, &bitfield);
			value.data = &bitfield;
			value.size = 1;
		} else
			ret = cursor->get_value(cursor, &value);
	}
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: read_row: read row %" PRIu64 ": %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	/*
	 * Check for not-found status.
	 *
	 * In fixed length stores, zero values at the end of the key space
	 * are treated as not found.  Treat this the same as a zero value
	 * in the key space, to match BDB's behavior.
	 */
	if (g.c_file_type == FIX && ret == WT_NOTFOUND) {
		bitfield = 0;
		ret = 0;
	}

	NTF_CHK(notfound_chk("read_row", ret, notfound, keyno));

	/* Compare the two. */
	if (value.size != bdb_value.size ||
	    memcmp(value.data, bdb_value.data, value.size) != 0) {
		fprintf(stderr,
		    "read_row: read row value mismatch %" PRIu64 ":\n", keyno);
		print_item("bdb", &bdb_value);
		print_item(" wt", &value);
		return (1);
	}
	return (0);
}

/*
 * nextprev --
 *	Read and verify the next/prev element in a row- or column-store file.
 */
static int
nextprev(int next, int insert, int *notfoundp)
{
	static WT_ITEM key, value, bdb_key, bdb_value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t keyno;
	int notfound, ret;
	uint8_t bitfield;
	const char *which;
	char *p;

	cursor = insert ? g.wts_cursor_insert : g.wts_cursor;
	session = g.wts_session;
	which = next ? "next" : "prev";

	/* Retrieve the BDB value. */
	if (bdb_np(next, &bdb_key.data, &bdb_key.size,
	    &bdb_value.data, &bdb_value.size, &notfound))
		return (1);
	*notfoundp = notfound;

	keyno = 0;
	ret = next ? cursor->next(cursor) : cursor->prev(cursor);
	if (ret == 0)
		switch (g.c_file_type) {
		case FIX:
			if ((ret = cursor->get_key(cursor, &keyno)) == 0 &&
			    (ret = cursor->get_value(cursor, &bitfield)) == 0) {
				value.data = &bitfield;
				value.size = 1;
			}
			break;
		case ROW:
			if ((ret = cursor->get_key(cursor, &key)) == 0)
				ret = cursor->get_value(cursor, &value);
			break;
		case VAR:
			if ((ret = cursor->get_key(cursor, &keyno)) == 0)
				ret = cursor->get_value(cursor, &value);
			break;
		}
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
		    "%s: wts_%s: %s\n",
		    g.progname, which, wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(notfound_chk(
	    next ? "nextprev(next)" : "nextprev(prev)", ret, notfound, keyno));

	/* Compare the two. */
	if (g.c_file_type == ROW) {
		if (key.size != bdb_key.size ||
		    memcmp(key.data, bdb_key.data, key.size) != 0) {
			fprintf(stderr, "nextprev: %s key mismatch:\n", which);
			print_item("bdb-key", &bdb_key);
			print_item(" wt-key", &key);
			return (1);
		}
	} else {
		if (keyno != (uint64_t)atoll(bdb_key.data)) {
			if ((p = strchr((char *)bdb_key.data, '.')) != NULL)
				*p = '\0';
			fprintf(stderr,
			    "nextprev: %s key mismatch: %.*s != %" PRIu64 "\n",
			    which,
			    (int)bdb_key.size, (char *)bdb_key.data, keyno);
			return (1);
		}
	}
	if (value.size != bdb_value.size ||
	    memcmp(value.data, bdb_value.data, value.size) != 0) {
		fprintf(stderr, "nextprev: %s value mismatch:\n", which);
		print_item("bdb-value", &bdb_value);
		print_item(" wt-value", &value);
		return (1);
	}

	if (g.logging == LOG_OPS)
		switch (g.c_file_type) {
		case FIX:
			(void)session->msg_printf(
			    session, "%-10s%" PRIu64 " {0x%02x}", which,
			    keyno, ((char *)value.data)[0]);
			break;
		case ROW:
			(void)session->msg_printf(session, "%-10s{%.*s/%.*s}",
			    which,
			    (int)key.size, (char *)key.data,
			    (int)value.size, (char *)value.data);
			break;
		case VAR:
			(void)session->msg_printf(
			    session, "%-10s%" PRIu64 " {%.*s}",
			    which, keyno, (int)value.size, (char *)value.data);
			break;
		}

	return (0);
}

/*
 * row_put --
 *	Update an element in a row-store file.
 */
static int
row_put(uint64_t keyno, int insert)
{
	static WT_ITEM key, value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, insert);
	value_gen(&value.data, &value.size, keyno);

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)session->msg_printf(session, "%-10s{%.*s}\n%-10s{%.*s}",
		    insert ? "insertK" : "putK",
		    (int)key.size, (char *)key.data,
		    insert ? "insertV" : "putV",
		    (int)value.size, (char *)value.data);

	if (bdb_put(key.data, key.size, value.data, value.size, &notfound))
		return (1);

	cursor->set_key(cursor, &key);
	cursor->set_value(cursor, &value);
	ret = cursor->insert(cursor);
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
		    "%s: row_put: %s row %" PRIu64 " by key: %s\n",
		    g.progname, insert ? "insert" : "update",
		    keyno, wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(notfound_chk("row_put", ret, notfound, keyno));
	return (0);
}

/*
 * col_put --
 *	Update an element in a column-store file.
 */
static int
col_put(uint64_t keyno)
{
	static WT_ITEM key, value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, 0);
	value_gen(&value.data, &value.size, keyno);

	/* Log the operation */
	if (g.logging == LOG_OPS) {
		if (g.c_file_type == FIX)
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "update", keyno,
			    ((uint8_t *)value.data)[0]);
		else
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {%.*s}",
			    "update", keyno,
			    (int)value.size, (char *)value.data);
	}

	cursor->set_key(cursor, keyno);
	if (g.c_file_type == FIX)
		cursor->set_value(cursor, *(uint8_t *)value.data);
	else
		cursor->set_value(cursor, &value);
	ret = cursor->insert(cursor);
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
		    "%s: col_put: %" PRIu64 " : %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	if (bdb_put(key.data, key.size, value.data, value.size, &notfound))
		return (1);

	NTF_CHK(notfound_chk("col_put", ret, notfound, keyno));
	return (0);
}

/*
 * col_insert --
 *	Insert an element in a column-store file.
 */
static int
col_insert(uint64_t *keynop)
{
	static WT_ITEM key, value;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t keyno;
	int notfound, ret;

	/* Reset the other cursor so it doesn't keep pages pinned. */
	cursor = g.wts_cursor;
	cursor->reset(cursor);

	cursor = g.wts_cursor_insert;
	session = g.wts_session;

	value_gen(&value.data, &value.size, g.rows + 1);

	if (g.c_file_type == FIX)
		cursor->set_value(cursor, *(uint8_t *)value.data);
	else
		cursor->set_value(cursor, &value);
	ret = cursor->insert(cursor);
	if (ret != 0) {
		fprintf(stderr, "%s: col_insert: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	if ((ret = cursor->get_key(cursor, &keyno)) != 0) {
		fprintf(stderr, "%s: cursor->get_key: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	if (keyno <= g.rows) {
		fprintf(stderr,
		    "%s: inserted key did not create new row\n", g.progname);
		return (1);
	}
	*keynop = g.rows = (uint32_t)keyno;

	if (g.logging == LOG_OPS) {
		if (g.c_file_type == FIX)
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "insert", keyno,
			    ((uint8_t *)value.data)[0]);
		else
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {%.*s}",
			    "insert", keyno,
			    (int)value.size, (char *)value.data);
	}

	key_gen(&key.data, &key.size, keyno, 0);
	return (bdb_put(
	    key.data, key.size, value.data, value.size, &notfound) ? 1 : 0);
}

/*
 * row_del --
 *	Delete an element from a row-store file.
 */
static int
row_del(uint64_t keyno, int *notfoundp)
{
	static WT_ITEM key;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	*notfoundp = 0;
	cursor = g.wts_cursor;
	session = g.wts_session;

	key_gen(&key.data, &key.size, keyno, 0);

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)session->msg_printf(
		    session, "%-10s%" PRIu64, "remove", keyno);

	if (bdb_del(keyno, &notfound))
		return (1);
	*notfoundp = notfound;

	cursor->set_key(cursor, &key);
	ret = cursor->remove(cursor);
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
		    "%s: row_del: remove %" PRIu64 " by key: %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(notfound_chk("row_del", ret, notfound, keyno));
	return (0);
}

/*
 * col_del --
 *	Delete an element from a column-store file.
 */
static int
col_del(uint64_t keyno, int *notfoundp)
{
	static WT_ITEM key;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int notfound, ret;

	cursor = g.wts_cursor;
	session = g.wts_session;

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)session->msg_printf(
		    session, "%-10s%" PRIu64, "remove", keyno);

	/*
	 * Deleting a fixed-length item is the same as setting the bits to 0;
	 * do the same thing for the BDB store.
	 */
	if (g.c_file_type == FIX) {
		key_gen(&key.data, &key.size, keyno, 0);
		if (bdb_put(key.data, key.size, "\0", 1, &notfound))
			return (1);
	} else {
		if (bdb_del(keyno, &notfound))
			return (1);
		*notfoundp = notfound;
	}

	cursor->set_key(cursor, keyno);
	ret = cursor->remove(cursor);
	if (ret != 0 && ret != WT_NOTFOUND) {
		fprintf(stderr,
		    "%s: col_del: remove %" PRIu64 " by key: %s\n",
		    g.progname, keyno, wiredtiger_strerror(ret));
		return (1);
	}

	NTF_CHK(notfound_chk("col_del", ret, notfound, keyno));
	return (0);
}

/*
 * notfound_chk --
 *	Compare notfound returns for consistency.
 */
static int
notfound_chk(const char *f, int wt_ret, int bdb_notfound, uint64_t keyno)
{
	/* Check for not found status. */
	if (bdb_notfound) {
		if (wt_ret == WT_NOTFOUND)
			return (2);

		fprintf(stderr, "%s: %s:", g.progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " not found in Berkeley DB, found in WiredTiger\n");
		return (1);
	}
	if (wt_ret == WT_NOTFOUND) {
		fprintf(stderr, "%s: %s:", g.progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " found in Berkeley DB, not found in WiredTiger\n");
		return (1);
	}
	return (0);
}

/*
 * print_item --
 *	Display a single data/size pair, with a tag.
 */
static void
print_item(const char *tag, WT_ITEM *item)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *data;
	uint32_t size;
	int ch;

	data = item->data;
	size = item->size;

	fprintf(stderr, "\t%s {", tag);
	if (g.c_file_type == FIX)
		fprintf(stderr, "0x%02x", data[0]);
	else
		for (; size > 0; --size, ++data) {
			ch = data[0];
			if (isprint(ch))
				fprintf(stderr, "%c", ch);
			else
				fprintf(stderr, "%x%x",
				    hex[(data[0] & 0xf0) >> 4],
				    hex[data[0] & 0x0f]);
		}
	fprintf(stderr, "}\n");
}
