/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "format.h"

static void  col_insert(WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t *);
static void  col_remove(WT_CURSOR *, WT_ITEM *, uint64_t, int *);
static void  col_update(WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t);
static void  nextprev(WT_CURSOR *, int, int *);
static int   notfound_chk(const char *, int, int, uint64_t);
static void *ops(void *);
static void  print_item(const char *, WT_ITEM *);
static void  read_row(WT_CURSOR *, WT_ITEM *, uint64_t);
static void  row_remove(WT_CURSOR *, WT_ITEM *, uint64_t, int *);
static void  row_update(WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t, int);

/*
 * wts_ops --
 *	Perform a number of operations in a set of threads.
 */
void
wts_ops(void)
{
	TINFO *tinfo, total;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	time_t now;
	int ret, running;
	uint32_t i;

	conn = g.wts_conn;

	/* Open a session. */
	session = NULL;
	if (g.logging == LOG_OPS) {
		if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
			die(ret, "connection.open_session");

		(void)time(&now);
		(void)session->msg_printf(session,
		    "===============\nthread ops start: %s===============",
		    ctime(&now));
	}

	if (SINGLETHREADED) {
		memset(&total, 0, sizeof(total));
		total.id = 1;
		(void)ops(&total);
	} else {
		/* Create thread structure. */
		if ((tinfo =
		    calloc((size_t)g.c_threads, sizeof(*tinfo))) == NULL)
			die(errno, "calloc");
		for (i = 0; i < g.c_threads; ++i) {
			tinfo[i].id = (int)i + 1;
			tinfo[i].state = TINFO_RUNNING;
			if ((ret = pthread_create(
			    &tinfo[i].tid, NULL, ops, &tinfo[i])) != 0)
				die(ret, "pthread_create");
		}

		/* Wait for the threads. */
		for (;;) {
			total.search =
			    total.insert = total.remove = total.update = 0;
			for (i = running = 0; i < g.c_threads; ++i) {
				total.search += tinfo[i].search;
				total.insert += tinfo[i].insert;
				total.remove += tinfo[i].remove;
				total.update += tinfo[i].update;
				switch (tinfo[i].state) {
				case TINFO_RUNNING:
					running = 1;
					break;
				case TINFO_COMPLETE:
					tinfo[i].state = TINFO_JOINED;
					(void)pthread_join(tinfo[i].tid, NULL);
					break;
				case TINFO_JOINED:
					break;
				}
			}
			track("read/write ops", 0ULL, &total);
			if (!running)
				break;
			(void)usleep(100000);		/* 1/10th of a second */
		}
		free(tinfo);
	}

	if (session != NULL) {
		(void)time(&now);
		(void)session->msg_printf(session,
		    "===============\nthread ops stop: %s===============",
		    ctime(&now));

		if ((ret = session->close(session, NULL)) != 0)
			die(ret, "session.close");
	}
}

static void *
ops(void *arg)
{
	TINFO *tinfo;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor, *cursor_insert;
	WT_SESSION *session;
	WT_ITEM key, value;
	uint64_t cnt, keyno, sync_op;
	uint32_t op;
	uint8_t *keybuf, *valbuf;
	u_int np;
	int dir, insert, notfound, ret, sync_drop;
	char sync_name[64];

	conn = g.wts_conn;

	tinfo = arg;

	/* Set up the default key and value buffers. */
	memset(&key, 0, sizeof(key));
	key_gen_setup(&keybuf);
	memset(&value, 0, sizeof(value));
	val_gen_setup(&valbuf);

	/* Open a session. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");

	/*
	 * Open two cursors: one configured for overwriting and one configured
	 * for append if we're dealing with a column-store.
	 *
	 * The reason is when testing with existing records, we don't track if
	 * a record was deleted or not, which means we must use cursor->insert
	 * with overwriting configured.  But, in column-store files where we're
	 * testing with new, appended records, we don't want to have to specify
	 * the record number, which requires an append configuration.
	 */
	cursor = cursor_insert = NULL;
	if ((ret = session->open_cursor(session,
	    WT_TABLENAME, NULL, "overwrite", &cursor)) != 0)
		die(ret, "session.open_cursor");
	if ((g.c_file_type == FIX || g.c_file_type == VAR) &&
	    (ret = session->open_cursor(session,
	    WT_TABLENAME, NULL, "append", &cursor_insert)) != 0)
		die(ret, "session.open_cursor");

	/* Pick an operation where we'll do a sync and create the name. */
	sync_drop = 0;
	sync_op = MMRAND(1, g.c_ops);
	snprintf(sync_name, sizeof(sync_name), "snapshot=thread-%d", tinfo->id);

	for (cnt = 0; cnt < g.c_ops / g.c_threads; ++cnt) {
		if (SINGLETHREADED && cnt % 100 == 0)
			track("read/write ops", 0ULL, tinfo);

		if (cnt == sync_op) {
			if (sync_drop && (int)MMRAND(1, 4) == 1) {
				if ((ret = session->drop(
				    session, WT_TABLENAME, sync_name)) != 0)
					die(ret, "session.drop: %s: %s",
					    WT_TABLENAME, sync_name);
				sync_drop = 0;
			} else {
				if ((ret = session->sync(
				    session, WT_TABLENAME, sync_name)) != 0)
					die(ret, "session.sync: %s: %s",
					    WT_TABLENAME, sync_name);
				sync_drop = 1;
			}

			/* Pick the next sync operation. */
			sync_op += wts_rand() % 1000;
		}

		insert = notfound = 0;

		keyno = MMRAND(1, g.rows);
		key.data = keybuf;
		value.data = valbuf;

		/*
		 * Perform some number of operations: the percentage of deletes,
		 * inserts and writes are specified, reads are the rest.  The
		 * percentages don't have to add up to 100, a high percentage
		 * of deletes will mean fewer inserts and writes.  A read
		 * operation always follows a modification to confirm it worked.
		 */
		op = (uint32_t)(wts_rand() % 100);
		if (op < g.c_delete_pct) {
			++tinfo->remove;
			switch (g.c_file_type) {
			case ROW:
				/*
				 * If deleting a non-existent record, the cursor
				 * won't be positioned, and so can't do a next.
				 */
				row_remove(cursor, &key, keyno, &notfound);
				break;
			case FIX:
			case VAR:
				col_remove(cursor, &key, keyno, &notfound);
				break;
			}
		} else if (op < g.c_delete_pct + g.c_insert_pct) {
			++tinfo->insert;
			switch (g.c_file_type) {
			case ROW:
				row_update(cursor, &key, &value, keyno, 1);
				break;
			case FIX:
			case VAR:
				/*
				 * Reset the standard cursor so it doesn't keep
				 * pages pinned.
				 */
				if ((ret = cursor->reset(cursor)) != 0)
					die(ret, "cursor.reset");
				col_insert(cursor_insert, &key, &value, &keyno);
				insert = 1;
				break;
			}
		} else if (
		    op < g.c_delete_pct + g.c_insert_pct + g.c_write_pct) {
			++tinfo->update;
			switch (g.c_file_type) {
			case ROW:
				row_update(cursor, &key, &value, keyno, 0);
				break;
			case FIX:
			case VAR:
				col_update(cursor, &key, &value, keyno);
				break;
			}
		} else {
			++tinfo->search;
			read_row(cursor, &key, keyno);
			continue;
		}

		/*
		 * If we did any operation, we've set the cursor, do a small
		 * number of next/prev cursor operations in a random direction.
		 */
		dir = (int)MMRAND(0, 1);
		for (np = 0; np < MMRAND(1, 8); ++np) {
			if (notfound)
				break;
			nextprev(
			    insert ? cursor_insert : cursor, dir, &notfound);
		}

		if (insert && (ret = cursor_insert->reset(cursor_insert)) != 0)
			die(ret, "cursor.reset");

		/* Read the value we modified to confirm the operation. */
		read_row(cursor, &key, keyno);
	}

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");

	free(keybuf);
	free(valbuf);

	tinfo->state = TINFO_COMPLETE;
	return (NULL);
}

/*
 * wts_read_scan --
 *	Read and verify all elements in a file.
 */
void
wts_read_scan(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_ITEM key;
	WT_SESSION *session;
	uint64_t cnt, last_cnt;
	uint8_t *keybuf;
	int ret;

	conn = g.wts_conn;

	/* Set up the default key buffer. */
	memset(&key, 0, sizeof(key));
	key_gen_setup(&keybuf);

	/* Open a session and cursor pair. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");
	if ((ret = session->open_cursor(
	    session, WT_TABLENAME, NULL, NULL, &cursor)) != 0)
		die(ret, "session.open_cursor");

	/* Check a random subset of the records using the key. */
	for (last_cnt = cnt = 0; cnt < g.key_cnt;) {
		cnt += wts_rand() % 17 + 1;
		if (cnt > g.rows)
			cnt = g.rows;
		if (cnt - last_cnt > 1000) {
			track("read row scan", cnt, NULL);
			last_cnt = cnt;
		}

		key.data = keybuf;
		read_row(cursor, &key, cnt);
	}

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");

	free(keybuf);
}

#define	NTF_CHK(a) do {							\
	switch (a) {							\
	case 0:								\
		break;							\
	case 1:								\
		return;							\
	}								\
} while (0)

/*
 * read_row --
 *	Read and verify a single element in a row- or column-store file.
 */
static void
read_row(WT_CURSOR *cursor, WT_ITEM *key, uint64_t keyno)
{
	WT_ITEM bdb_value, value;
	WT_SESSION *session;
	int notfound, ret;
	uint8_t bitfield;

	session = cursor->session;

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)session->msg_printf(
		    session, "%-10s%" PRIu64, "read", keyno);

	/* Retrieve the key/value pair by key. */
	switch (g.c_file_type) {
	case FIX:
	case VAR:
		cursor->set_key(cursor, keyno);
		break;
	case ROW:
		key_gen((uint8_t *)key->data, &key->size, keyno, 0);
		cursor->set_key(cursor, key);
		break;
	}

	if ((ret = cursor->search(cursor)) == 0) {
		if (g.c_file_type == FIX) {
			ret = cursor->get_value(cursor, &bitfield);
			value.data = &bitfield;
			value.size = 1;
		} else {
			memset(&value, 0, sizeof(value));
			ret = cursor->get_value(cursor, &value);
		}
	}
	if (ret != 0 && ret != WT_NOTFOUND)
		die(ret, "read_row: read row %" PRIu64, keyno);

	if (!SINGLETHREADED)
		return;

	/* Retrieve the BDB value. */
	memset(&bdb_value, 0, sizeof(bdb_value));
	bdb_read(keyno, &bdb_value.data, &bdb_value.size, &notfound);

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
		die(0, NULL);
	}
}

/*
 * nextprev --
 *	Read and verify the next/prev element in a row- or column-store file.
 */
static void
nextprev(WT_CURSOR *cursor, int next, int *notfoundp)
{
	WT_ITEM key, value, bdb_key, bdb_value;
	WT_SESSION *session;
	uint64_t keyno;
	int notfound, ret;
	uint8_t bitfield;
	const char *which;
	char *p;

	session = cursor->session;
	which = next ? "next" : "prev";

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
	if (ret != 0 && ret != WT_NOTFOUND)
		die(ret, "%s", which);
	*notfoundp = ret == WT_NOTFOUND;

	if (!SINGLETHREADED)
		return;

	/* Retrieve the BDB value. */
	bdb_np(next, &bdb_key.data, &bdb_key.size,
	    &bdb_value.data, &bdb_value.size, &notfound);
	NTF_CHK(notfound_chk(
	    next ? "nextprev(next)" : "nextprev(prev)", ret, notfound, keyno));

	/* Compare the two. */
	if (g.c_file_type == ROW) {
		if (key.size != bdb_key.size ||
		    memcmp(key.data, bdb_key.data, key.size) != 0) {
			fprintf(stderr, "nextprev: %s key mismatch:\n", which);
			print_item("bdb-key", &bdb_key);
			print_item(" wt-key", &key);
			die(0, NULL);
		}
	} else {
		if (keyno != (uint64_t)atoll(bdb_key.data)) {
			if ((p = strchr((char *)bdb_key.data, '.')) != NULL)
				*p = '\0';
			fprintf(stderr,
			    "nextprev: %s key mismatch: %.*s != %" PRIu64 "\n",
			    which,
			    (int)bdb_key.size, (char *)bdb_key.data, keyno);
			die(0, NULL);
		}
	}
	if (value.size != bdb_value.size ||
	    memcmp(value.data, bdb_value.data, value.size) != 0) {
		fprintf(stderr, "nextprev: %s value mismatch:\n", which);
		print_item("bdb-value", &bdb_value);
		print_item(" wt-value", &value);
		die(0, NULL);
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
}

/*
 * row_update --
 *	Update a row in a row-store file.
 */
static void
row_update(
    WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, uint64_t keyno, int insert)
{
	WT_SESSION *session;
	int notfound, ret;

	session = cursor->session;

	key_gen((uint8_t *)key->data, &key->size, keyno, insert);
	value_gen((uint8_t *)value->data, &value->size, keyno);

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)session->msg_printf(session, "%-10s{%.*s}\n%-10s{%.*s}",
		    insert ? "insertK" : "putK",
		    (int)key->size, (char *)key->data,
		    insert ? "insertV" : "putV",
		    (int)value->size, (char *)value->data);

	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);
	if (ret != 0 && ret != WT_NOTFOUND)
		die(ret,
		    "row_update: %s row %" PRIu64 " by key",
		    insert ? "insert" : "update", keyno);

	if (!SINGLETHREADED)
		return;

	bdb_update(key->data, key->size, value->data, value->size, &notfound);
	NTF_CHK(notfound_chk("row_update", ret, notfound, keyno));
}

/*
 * col_update --
 *	Update a row in a column-store file.
 */
static void
col_update(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, uint64_t keyno)
{
	WT_SESSION *session;
	int notfound, ret;

	session = cursor->session;

	value_gen((uint8_t *)value->data, &value->size, keyno);

	/* Log the operation */
	if (g.logging == LOG_OPS) {
		if (g.c_file_type == FIX)
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "update", keyno,
			    ((uint8_t *)value->data)[0]);
		else
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {%.*s}",
			    "update", keyno,
			    (int)value->size, (char *)value->data);
	}

	cursor->set_key(cursor, keyno);
	if (g.c_file_type == FIX)
		cursor->set_value(cursor, *(uint8_t *)value->data);
	else
		cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);
	if (ret != 0 && ret != WT_NOTFOUND)
		die(ret, "col_update: %" PRIu64, keyno);

	if (!SINGLETHREADED)
		return;

	key_gen((uint8_t *)key->data, &key->size, keyno, 0);
	bdb_update(key->data, key->size, value->data, value->size, &notfound);
	NTF_CHK(notfound_chk("col_update", ret, notfound, keyno));
}

/*
 * col_insert --
 *	Insert an element in a column-store file.
 */
static void
col_insert(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, uint64_t *keynop)
{
	WT_SESSION *session;
	uint64_t keyno;
	int notfound, ret;

	session = cursor->session;

	value_gen((uint8_t *)value->data, &value->size, g.rows + 1);

	if (g.c_file_type == FIX)
		cursor->set_value(cursor, *(uint8_t *)value->data);
	else
		cursor->set_value(cursor, value);
	if ((ret = cursor->insert(cursor)) != 0)
		die(ret, "cursor.insert");
	if ((ret = cursor->get_key(cursor, &keyno)) != 0)
		die(ret, "cursor.get_key");
	*keynop = (uint32_t)keyno;

	/*
	 * Assign the maximum number of rows to the returned key: that key may
	 * not be the current maximum value, if we race with another thread,
	 * but that's OK, we just want it to keep increasing so we don't ignore
	 * records at the end of the table.
	 */
	g.rows = (uint32_t)keyno;

	if (g.logging == LOG_OPS) {
		if (g.c_file_type == FIX)
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "insert", keyno,
			    ((uint8_t *)value->data)[0]);
		else
			(void)session->msg_printf(session,
			    "%-10s%" PRIu64 " {%.*s}",
			    "insert", keyno,
			    (int)value->size, (char *)value->data);
	}

	if (!SINGLETHREADED)
		return;

	key_gen((uint8_t *)key->data, &key->size, keyno, 0);
	bdb_update(key->data, key->size, value->data, value->size, &notfound);
}

/*
 * row_remove --
 *	Remove an row from a row-store file.
 */
static void
row_remove(WT_CURSOR *cursor, WT_ITEM *key, uint64_t keyno, int *notfoundp)
{
	WT_SESSION *session;
	int notfound, ret;

	session = cursor->session;

	key_gen((uint8_t *)key->data, &key->size, keyno, 0);

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)session->msg_printf(
		    session, "%-10s%" PRIu64, "remove", keyno);

	cursor->set_key(cursor, key);
	ret = cursor->remove(cursor);
	if (ret != 0 && ret != WT_NOTFOUND)
		die(ret, "row_remove: remove %" PRIu64 " by key", keyno);
	*notfoundp = ret == WT_NOTFOUND;

	if (!SINGLETHREADED)
		return;

	bdb_remove(keyno, &notfound);
	NTF_CHK(notfound_chk("row_remove", ret, notfound, keyno));
}

/*
 * col_remove --
 *	Remove a row from a column-store file.
 */
static void
col_remove(WT_CURSOR *cursor, WT_ITEM *key, uint64_t keyno, int *notfoundp)
{
	WT_SESSION *session;
	int notfound, ret;

	session = cursor->session;

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)session->msg_printf(
		    session, "%-10s%" PRIu64, "remove", keyno);

	cursor->set_key(cursor, keyno);
	ret = cursor->remove(cursor);
	if (ret != 0 && ret != WT_NOTFOUND)
		die(ret, "col_remove: remove %" PRIu64 " by key", keyno);
	*notfoundp = ret == WT_NOTFOUND;

	if (!SINGLETHREADED)
		return;

	/*
	 * Deleting a fixed-length item is the same as setting the bits to 0;
	 * do the same thing for the BDB store.
	 */
	if (g.c_file_type == FIX) {
		key_gen((uint8_t *)key->data, &key->size, keyno, 0);
		bdb_update(key->data, key->size, "\0", 1, &notfound);
	} else
		bdb_remove(keyno, &notfound);

	NTF_CHK(notfound_chk("col_remove", ret, notfound, keyno));
}

/*
 * notfound_chk --
 *	Compare notfound returns for consistency.
 */
static int
notfound_chk(const char *f, int wt_ret, int bdb_notfound, uint64_t keyno)
{
	/* Check for not found status. */
	if (bdb_notfound && wt_ret == WT_NOTFOUND)
		return (1);

	if (bdb_notfound) {
		fprintf(stderr, "%s: %s:", g.progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " not found in Berkeley DB, found in WiredTiger\n");
		die(0, NULL);
	}
	if (wt_ret == WT_NOTFOUND) {
		fprintf(stderr, "%s: %s:", g.progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " found in Berkeley DB, not found in WiredTiger\n");
		die(0, NULL);
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
