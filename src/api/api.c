/* Copyright (c) 2010 WiredTiger, Inc.  All rights reserved. */

#include <sys/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wt_int.h"

static int
__cursor_first(WT_CURSOR *cursor)
{
	return (ENOTSUP);
}

static int
__cursor_last(WT_CURSOR *cursor)
{
	return (ENOTSUP);
}

static int
__cursor_next(WT_CURSOR *cursor)
{
	return (ENOTSUP);
}

static int
__cursor_prev(WT_CURSOR *cursor)
{
	return (ENOTSUP);
}

static int
__cursor_search(WT_CURSOR *cursor, int *exact)
{
	return (ENOTSUP);
}

static int
__cursor_insert(WT_CURSOR *cursor)
{
	return (ENOTSUP);
}

static int
__cursor_update(WT_CURSOR *cursor)
{
	return (ENOTSUP);
}

static int
__cursor_del(WT_CURSOR *cursor)
{
	return (ENOTSUP);
}

static int
__cursor_close(WT_CURSOR *cursor, const char *config)
{
	WT_CURSOR_STD *cstd = (WT_CURSOR_STD *)cursor;
	__wt_curstd_close(cstd);
	free(cursor);
	return (0);
}

static int
__session_close(WT_SESSION *session, const char *config)
{
	printf("WT_SESSION->close\n");
	free(session);
	return (0);
}

static int
__session_open_cursor(WT_SESSION *session, const char *uri, const char *config, WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		__cursor_first,
		__cursor_last,
		__cursor_next,
		__cursor_prev,
		__cursor_search,
		__cursor_insert,
		__cursor_update,
		__cursor_del,
		__cursor_close,
	};
	WT_CURSOR_STD *cstd = (WT_CURSOR_STD *)malloc(sizeof(WT_CURSOR_STD));
	WT_CURSOR *c = &cstd->interface;

	printf("WT_SESSION->open_cursor\n");
	if (c == NULL)
		return (ENOMEM);
	*c = iface;
	c->session = session;
	c->key_format = c->value_format = "u";
	__wt_curstd_init(cstd);
	*cursorp = c;

	return (0);
}

static int
__session_dup_cursor(WT_SESSION *session, WT_CURSOR *cursor, const char *config, WT_CURSOR **dupp)
{
	return (ENOTSUP);
}

static int
__session_add_schema(WT_SESSION *session, const char *name, const char *config)
{
	return (ENOTSUP);
}

static int
__session_create_table(WT_SESSION *session, const char *name, const char *config)
{
	printf("WT_SESSION->create_table\n");
	return (ENOTSUP);
}

static int
__session_rename_table(WT_SESSION *session, const char *oldname, const char *newname, const char *config)
{
	return ENOTSUP;
}

static int
__session_drop_table(WT_SESSION *session, const char *name, const char *config)
{
	return ENOTSUP;
}

static int
__session_truncate_table(WT_SESSION *session, const char *name, WT_CURSOR *start, WT_CURSOR *end, const char *config)
{
	return ENOTSUP;
}

static int
__session_verify_table(WT_SESSION *session, const char *name, const char *config)
{
	return 0;
}

static int
__session_begin_transaction(WT_SESSION *session, const char *config)
{
	return ENOTSUP;
}

static int
__session_commit_transaction(WT_SESSION *session)
{
	return ENOTSUP;
}

static int
__session_rollback_transaction(WT_SESSION *session)
{
	return ENOTSUP;
}

static int
__session_checkpoint(WT_SESSION *session, const char *config)
{
	return ENOTSUP;
}

static int
__conn_load_extension(WT_CONNECTION *connection, const char *path, const char *config)
{
	return ENOTSUP;
}

static int
__conn_add_cursor_factory(WT_CONNECTION *connection, const char *prefix, WT_CURSOR_FACTORY *factory, const char *config)
{
	return ENOTSUP;
}

static int
__conn_add_collator(WT_CONNECTION *connection, const char *name, WT_COLLATOR *collator, const char *config)
{
	return ENOTSUP;
}

static int
__conn_add_extractor(WT_CONNECTION *connection, const char *name, WT_EXTRACTOR *extractor, const char *config)
{
	return ENOTSUP;
}

static const char *
__conn_get_home(WT_CONNECTION *conn)
{
	return NULL;
}

static int
__conn_is_new(WT_CONNECTION *conn)
{
	return 0;
}

static int
__conn_close(WT_CONNECTION *conn, const char *config)
{
	printf("WT_CONNECTION->close\n");
	free(conn);
	return 0;
}

static int
__conn_open_session(WT_CONNECTION *connection, WT_ERROR_HANDLER *errhandler, const char *config, WT_SESSION **sessionp)
{
	WT_SESSION stds = {
		NULL,
		__session_close,
		__session_open_cursor,
		__session_dup_cursor,
		__session_add_schema,
		__session_create_table,
		__session_rename_table,
		__session_drop_table,
		__session_truncate_table,
		__session_verify_table,
		__session_begin_transaction,
		__session_commit_transaction,
		__session_rollback_transaction,
		__session_checkpoint,
	};
	WT_SESSION *s = (WT_SESSION *)malloc(sizeof(WT_SESSION));

	printf("WT_CONNECTION->open_session\n");
	if (s == NULL)
		return ENOMEM;
	*s = stds;
	s->connection = connection;
	*sessionp = s;
	return 0;
}

int
wiredtiger_open(const char *home, WT_ERROR_HANDLER *errhandler, const char *config, WT_CONNECTION **connectionp)
{
	WT_CONNECTION stdc = {
		__conn_load_extension,
		__conn_add_cursor_factory,
		__conn_add_collator,
		__conn_add_extractor,
		__conn_close,
		__conn_get_home,
		__conn_is_new,
		__conn_open_session
	};
	WT_CONNECTION *c = (WT_CONNECTION *)malloc(sizeof(WT_CONNECTION));

	printf("wiredtiger_open\n");
	if (c == NULL)
		return ENOMEM;
	*c = stdc;
	/* TODO: c->home = strdup(home); */
	*connectionp = c;
	return 0;
}

const char *
wiredtiger_strerror(int err)
{
	/* TODO */
	return "unknown error";
}

const char *
wiredtiger_version(int *majorp, int *minorp, int *patchp)
{
	/* TODO */
	*majorp = *minorp = 0;
	*patchp = 1;
	return "0.0.1";
}
