/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * ex_hello.c
 *	Containing a call to every method in the WiredTiger API.
 *
 *	It doesn't do anything very useful, just demonstrates how to call each
 *	method.  This file is used to populate the API reference with code
 *	fragments.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

void cursor_ops(WT_CURSOR *cursor);
void session_ops(WT_SESSION *session);
void add_factory(WT_CONNECTION *conn);
void add_collator(WT_CONNECTION *conn);
void add_extractor(WT_CONNECTION *conn);
void connection_ops(WT_CONNECTION *conn);

const char *home = "WT_TEST";

void
cursor_ops(WT_CURSOR *cursor)
{
	int exact, ret;
	const char *key;
	const char *value;

	ret = cursor->get_key(cursor, &key);
	ret = cursor->get_value(cursor, &value);
	printf("Got key %s, value %s\n", key, value);

	key = "another key";
	cursor->set_key(cursor, key);
	value = "another value";
	cursor->set_value(cursor, value);

	ret = cursor->first(cursor);
	ret = cursor->last(cursor);
	ret = cursor->next(cursor);
	ret = cursor->prev(cursor);

	cursor->set_key(cursor, key);
	ret = cursor->search(cursor);

	cursor->set_key(cursor, key);
	ret = cursor->search_near(cursor, &exact);
	if (ret == 0) {
		/*
		 * if (exact > 0), we found a match greater than the search key,
		 * if (exact < 0), we found a match less than the search key,
		 * otherwise, exact == 0 and we found an exact match.
		 */
	}

	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);

	cursor->set_value(cursor, value);
	ret = cursor->update(cursor);

	ret = cursor->del(cursor);

	ret = cursor->close(cursor, NULL);
}

void
session_ops(WT_SESSION *session)
{
	int ret;

	WT_CURSOR *cursor;
	ret = session->open_cursor(session, "table:mytable", NULL, NULL, &cursor);

	cursor_ops(cursor);

	ret = session->create_table(session, "mytable", "key_format=S,value_format=S");

	ret = session->rename_table(session, "oldtable", "newtable", NULL);

	ret = session->drop_table(session, "mytable", NULL);

	ret = session->truncate_table(session, "mytable", NULL, NULL, NULL);

	ret = session->verify_table(session, "mytable", NULL);

	ret = session->begin_transaction(session, NULL);

	ret = session->commit_transaction(session, NULL);

	ret = session->rollback_transaction(session, NULL);

	ret = session->checkpoint(session, NULL);

	ret = session->close(session, NULL);
}

/* Implementation of WT_CURSOR_FACTORY for WT_CONNECTION::add_cursor_factory. */
static int
my_cursor_size(WT_CURSOR_FACTORY *factory, const char *obj, size_t *sizep)
{
	(void)factory;
	(void)obj;

	*sizep = sizeof (WT_CURSOR);
	return (0);
}

static int
my_init_cursor(WT_CURSOR_FACTORY *factory, WT_SESSION *session,
    const char *obj, WT_CURSOR *old_cursor, const char *config,
    WT_CURSOR *new_cursor)
{
	/* Unused parameters */
	(void)factory;
	(void)session;
	(void)obj;
	(void)old_cursor;
	(void)config;
	(void)new_cursor;

	return (0);
}
/* End implementation of WT_CURSOR_FACTORY. */

void
add_factory(WT_CONNECTION *conn)
{
	int ret;

	static WT_CURSOR_FACTORY my_factory;
	my_factory.cursor_size = my_cursor_size;
	my_factory.init_cursor = my_init_cursor;
	ret = conn->add_cursor_factory(conn, NULL, &my_factory, NULL);
}

/* Implementation of WT_COLLATOR for WT_CONNECTION::add_collator. */
static int
my_compare(WT_SESSION *session, WT_COLLATOR *collator,
    const WT_DATAITEM *value1, const WT_DATAITEM *value2, int *cmp)
{
	/* Unused parameters */
	(void)session;
	(void)collator;

	*cmp = strcmp((const char *)value1->data, (const char *)value2->data);
	return (0);
}
/* End implementation of WT_COLLATOR. */

void
add_collator(WT_CONNECTION *conn)
{
	int ret;

	static WT_COLLATOR my_collator;
	my_collator.compare = my_compare;
	ret = conn->add_collator(conn, "my_collator", &my_collator, NULL);
}

/* Implementation of WT_EXTRACTOR for WT_CONNECTION::add_extractor. */
static int
my_extract(WT_SESSION *session, WT_EXTRACTOR *extractor,
    const WT_DATAITEM *key, const WT_DATAITEM *value,
    WT_DATAITEM *result)
{
	/* Unused parameters */
	(void)session;
	(void)extractor;
	(void)key;

	*result = *value;
	return (0);
}
/* End implementation of WT_EXTRACTOR. */

void
add_extractor(WT_CONNECTION *conn)
{
	int ret;

	static WT_EXTRACTOR my_extractor;
	my_extractor.extract = my_extract;
	ret = conn->add_extractor(conn, "my_extractor", &my_extractor, NULL);
}

void
connection_ops(WT_CONNECTION *conn)
{
	int ret;

	ret = conn->load_extension(conn, "my_extension.dll", NULL);

	add_factory(conn);
	add_collator(conn);
	add_extractor(conn);

	ret = conn->close(conn, NULL);

	printf("The home is %s\n", conn->get_home(conn));

	if (conn->is_new(conn)) {
		/* First time initialization. */
	}

	WT_SESSION *session;
	ret = conn->open_session(conn, NULL, NULL, &session);

	session_ops(session);
};

int main() {
	int ret;

	WT_CONNECTION *conn;
	ret = wiredtiger_open("wt_data", NULL, "create,transactional", &conn);

	fprintf(stderr, "Error during operation: %s\n", wiredtiger_strerror(ret));

	size_t size;
	size = wiredtiger_struct_size("iSh", 42, "hello", -3);
	assert(size < 100);

	char buf[100];
	ret = wiredtiger_struct_pack(buf, sizeof (buf), "iSh", 42, "hello", -3);
 
	int i;
	char *s;
	short h;
	ret = wiredtiger_struct_unpack(buf, sizeof (buf), "iSh", &i, &s, &h);

	int major, minor, patch;
	printf("WiredTiger version %s\n",
	    wiredtiger_version(&major, &minor, &patch));

	return (0);
}
