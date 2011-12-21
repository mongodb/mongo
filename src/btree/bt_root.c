/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __btree_get_root(WT_SESSION_IMPL *, WT_BUF *);
static int __btree_get_root_turtle(WT_BUF *);
static int __btree_set_root(WT_SESSION_IMPL *, WT_BUF *);
static int __btree_set_root_turtle(WT_SESSION_IMPL *, WT_BUF *);

#define	WT_TURTLE_MSG		"The turtle.\n"

#define	WT_SCHEMA_TURTLE	"WiredTiger.turtle"	/* Schema root page */
#define	WT_SCHEMA_TURTLE_SET	"WiredTiger.turtle.set"	/* Schema root prep */

int
__wt_btree_get_root(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_BUF *value;
	uint32_t size;
	int ret;

	btree = session->btree;
	ret = 0;

	WT_RET(__wt_scr_alloc(session, 1024, &value));
	WT_ERR(strcmp(btree->filename, WT_SCHEMA_FILENAME) == 0 ?
	    __btree_get_root_turtle(value) : __btree_get_root(session, value));

	if ((size = value->size) != 0) {
		WT_ERR(__wt_hex_to_raw(session, value->mem, value->mem, &size));
		WT_ERR(
		    __wt_strndup(session, value->mem, size, &btree->root_addr));
		btree->root_size = size;
	}

	if (0) {
err:		__wt_err(session, ret,
		    "unable to find %s file's root address", btree->name);
	}

	__wt_scr_free(&value);

	return (ret);
}

int
__wt_btree_set_root(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_BUF *value;
	uint32_t size;
	int ret;

	btree = session->btree;
	value = NULL;

	/*
	 * In the worst case, every character takes up 3 spaces, plus a
	 * trailing nul byte.
	 */
	size = btree->root_size;
	WT_RET(__wt_scr_alloc(
	    session, size * 3 + (uint32_t)strlen(WT_NOADDR), &value));

	if (btree->root_addr == NULL) {
		value->data = WT_NOADDR;
		value->size = WT_STORE_SIZE(strlen(WT_NOADDR));
	} else {
		__wt_raw_to_hex(btree->root_addr, value->mem, &size);
		value->size = size;
	}

	ret = strcmp(btree->filename, WT_SCHEMA_FILENAME) == 0 ?
	    __btree_set_root_turtle(session, value) :
	    __btree_set_root(session, value);
	if (ret != 0)
		__wt_err(session, ret,
		    "unable to update %s file's root address", btree->name);

	__wt_scr_free(&value);

	return (ret);
}

static int
__btree_get_root_turtle(WT_BUF *value)
{
	FILE *fp;
	int ret;
	char *p;

	ret = 0;

	if ((fp = fopen(WT_SCHEMA_TURTLE, "r")) == NULL)
		return (0);
	if (fgets(value->mem, (int)value->memsize, fp) == NULL)
		goto err;
	if (strcmp(value->mem, WT_TURTLE_MSG) != 0)
		goto err;
	if (fgets(value->mem, (int)value->memsize, fp) == NULL)
		goto err;
	if ((p = strchr(value->mem, '\n')) == NULL)
		goto err;
	*p = '\0';
	value->size = WT_STORE_SIZE(strlen(value->mem));

	if (0) {
err:		ret = WT_ERROR;
	}
	WT_TRET(fclose(fp));

	return (ret);
}

static int
__btree_set_root_turtle(WT_SESSION_IMPL *session, WT_BUF *value)
{
	FILE *fp;
	size_t len;
	int ret;

	ret = 0;

	if ((fp = fopen(WT_SCHEMA_TURTLE_SET, "w")) == NULL)
		return (WT_ERROR);

	len = (size_t)
	    fprintf(fp, "%s%s\n", WT_TURTLE_MSG, (char *)value->data);
	if (len != strlen(WT_TURTLE_MSG) + (value->size - 1) + 1)
		ret = WT_ERROR;

	WT_TRET(fflush(fp));
	WT_TRET(fclose(fp));

	if (ret != 0) {
		(void)__wt_remove(session, WT_SCHEMA_TURTLE_SET);
		return (ret);
	}

	return (__wt_rename(session, WT_SCHEMA_TURTLE_SET, WT_SCHEMA_TURTLE));
}

static int
__btree_get_root(WT_SESSION_IMPL *session, WT_BUF *value)
{
	WT_SESSION *wt_session;
	WT_BTREE *btree;
	WT_BUF *key;
	WT_CURSOR *cursor;
	int ret;
	char *p;

	btree = session->btree;
	key = NULL;

	wt_session = (WT_SESSION *)session;
	if ((ret = wt_session->open_cursor(
	    wt_session, WT_SCHEMA_URI, NULL, NULL, &cursor)) != 0) {
		__wt_err(session, ret, "open_cursor: %s", WT_SCHEMA_URI);
		return (ret);
	}
	WT_ERR(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "root:%s", btree->filename));
	cursor->set_key(cursor, key->mem);
	switch (ret = cursor->search(cursor)) {
	case 0:
		ret = 0;
		WT_ERR(cursor->get_value(cursor, &p));
		WT_ERR(__wt_buf_set(session, value, p, strlen(p)));
		break;
	case WT_NOTFOUND:
		ret = 0;
		break;
	default:
		break;
	}

err:	WT_TRET(cursor->close(cursor, NULL));
	__wt_scr_free(&key);

	return (ret);
}

static int
__btree_set_root(WT_SESSION_IMPL *session, WT_BUF *value)
{
	WT_BTREE *btree;
	WT_BUF *key;
	WT_CURSOR *cursor;
	WT_SESSION *wt_session;
	int ret;

	btree = session->btree;
	key = NULL;

	wt_session = (WT_SESSION *)session;
	if ((ret = wt_session->open_cursor(
	    wt_session, WT_SCHEMA_URI, NULL, "overwrite", &cursor)) != 0) {
		__wt_err(session, ret, "open_cursor: %s", WT_SCHEMA_URI);
		return (ret);
	}
	WT_ERR(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "root:%s", btree->filename));
	cursor->set_key(cursor, key->mem);
	cursor->set_value(cursor, value->data);
	WT_ERR(cursor->insert(cursor));

err:	WT_TRET(cursor->close(cursor, NULL));
	__wt_scr_free(&key);

	return (ret);
}
