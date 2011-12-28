/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __btree_get_root(WT_SESSION_IMPL *, const char **);
static int __btree_get_root_turtle(WT_SESSION_IMPL *, const char **);
static int __btree_set_root(WT_SESSION_IMPL *, char *);
static int __btree_set_root_turtle(WT_SESSION_IMPL *, char *);

#define	WT_TURTLE_MSG		"The turtle.\n"

#define	WT_SCHEMA_TURTLE	"WiredTiger.turtle"	/* Schema root page */
#define	WT_SCHEMA_TURTLE_SET	"WiredTiger.turtle.set"	/* Schema root prep */

/*
 * __wt_btree_get_root --
 *	Get the file's root address.
 */
int
__wt_btree_get_root(WT_SESSION_IMPL *session, WT_BUF *addr)
{
	WT_BTREE *btree;
	uint32_t size;
	int ret;
	const char *v;

	btree = session->btree;
	ret = 0;

	/* If there's no root address, return a NULL with a size of 0. */
	addr->data = NULL;
	addr->size = 0;

	WT_ERR(strcmp(btree->filename, WT_SCHEMA_FILENAME) == 0 ?
	    __btree_get_root_turtle(session, &v) :
	    __btree_get_root(session, &v));

	/* Nothing or "[NoAddr]" means no address. */
	if (v != NULL && strlen(v) != 0 && strcmp(v, WT_NOADDR) != 0) {
		WT_ERR(__wt_hex_to_raw(session, (void *)v, (void *)v, &size));
		WT_ERR(__wt_buf_set(session, addr, v, size));
	} else if (v != NULL)
		__wt_free(session, v);

	if (0) {
err:		__wt_err(session, ret,
		    "unable to find %s file's root address", btree->filename);
	}

	return (ret);
}

/*
 * __wt_btree_free_root --
 *	Free the file's root address.
 */
int
__wt_btree_free_root(WT_SESSION_IMPL *session)
{
	WT_BUF *addr, *as;
	WT_BTREE *btree;
	int ret;

	btree = session->btree;
	addr = as = NULL;

	WT_RET(__wt_scr_alloc(session, WT_BM_MAX_ADDR_COOKIE, &addr));
	WT_ERR(__wt_btree_get_root(session, addr));
	if (addr->data != NULL) {
		WT_RET(__wt_scr_alloc(session, 0, &as));
		WT_VERBOSE(session, verify, "free %s root %s",
		    btree->filename,
		    __wt_addr_string(session, as, addr->data, addr->size));

		WT_ERR(__wt_bm_free(session, addr->data, addr->size));
	}

err:	__wt_scr_free(&addr);
	__wt_scr_free(&as);
	return (ret);
}

/*
 * __wt_btree_set_root --
 *	Set the file's root address.
 */
int
__wt_btree_set_root(WT_SESSION_IMPL *session, uint8_t *addr, uint32_t size)
{
	WT_BTREE *btree;
	WT_BUF *v;
	int ret;

	btree = session->btree;
	v = NULL;
	ret = 0;

	/*
	 * Every bytes is encoded as 2 bytes, plus a trailing nul byte,
	 * and it needs to hold the [NoAddr] string.
	 */
	WT_RET(__wt_scr_alloc(
	    session, size * 2 + WT_STORE_SIZE(strlen(WT_NOADDR)), &v));

	WT_VERBOSE(session, verify, "set %s root %s",
	    btree->filename, __wt_addr_string(session, v, addr, size));

	/*
	 * We're not using the WT_BUF as a buffer going forward, but fill
	 * in the values anyway, just for safety.
	 */
	if (addr == NULL) {
		v->data = WT_NOADDR;
		v->size = WT_STORE_SIZE(strlen(WT_NOADDR)) + 1;
	} else {
		__wt_raw_to_hex(addr, v->mem, &size);
		v->data = v->mem;
		v->size = size;
	}

	WT_ERR(strcmp(btree->filename, WT_SCHEMA_FILENAME) == 0 ?
	    __btree_set_root_turtle(session, (char *)v->data) :
	    __btree_set_root(session, (char *)v->data));

	if (0) {
err:		__wt_err(session, ret,
		    "unable to update %s file's root address", btree->filename);
	}

	__wt_scr_free(&v);

	return (ret);
}

/*
 * __btree_get_root --
 *	Get the schema file's root address.
 */
static int
__btree_get_root_turtle(WT_SESSION_IMPL *session, const char **vp)
{
	FILE *fp;
	int ret;
	const char *path;
	char *p, line[1024];

	*vp = NULL;

	fp = NULL;
	path = NULL;
	ret = 0;

	WT_ERR(__wt_filename(session, WT_SCHEMA_TURTLE, &path));
	WT_ERR_TEST((fp = fopen(path, "r")) == NULL, 0);
	WT_ERR_TEST(fgets(line, (int)sizeof(line), fp) == NULL, WT_ERROR);
	WT_ERR_TEST(strcmp(line, WT_TURTLE_MSG) != 0, WT_ERROR);
	WT_ERR_TEST(fgets(line, (int)sizeof(line), fp) == NULL, WT_ERROR);
	WT_ERR_TEST((p = strchr(line, '\n')) == NULL, WT_ERROR);
	*p = '\0';
	WT_ERR(__wt_strdup(session, line, vp));

err:	if (fp != NULL)
		WT_TRET(fclose(fp));
	if (path != NULL)
		__wt_free(session, path);

	return (ret);
}

/*
 * __btree_set_root --
 *	Set the schema file's root address.
 */
static int
__btree_set_root_turtle(WT_SESSION_IMPL *session, char *v)
{
	FILE *fp;
	size_t len;
	int ret;
	const char *path;

	ret = 0;
	path = NULL;

	WT_ERR(__wt_filename(session, WT_SCHEMA_TURTLE_SET, &path));
	WT_ERR_TEST((fp = fopen(path, "w")) == NULL, WT_ERROR);

	len = (size_t)fprintf(fp, "%s%s\n", WT_TURTLE_MSG, v);
	if (len != strlen(WT_TURTLE_MSG) + strlen(v) + 1)
		ret = WT_ERROR;

	WT_TRET(fflush(fp));
	WT_TRET(fclose(fp));

	if (ret == 0)
		ret = __wt_rename(
		    session, WT_SCHEMA_TURTLE_SET, WT_SCHEMA_TURTLE);
	else
		(void)__wt_remove(session, WT_SCHEMA_TURTLE_SET);

err:	if (path != NULL)
		__wt_free(session, path);
	return (ret);
}

/*
 * __btree_get_root --
 *	Get a non-schema file's root address.
 */
static int
__btree_get_root(WT_SESSION_IMPL *session, const char **vp)
{
	WT_BTREE *btree;
	WT_BUF *key;
	int ret;

	*vp = NULL;

	btree = session->btree;
	key = NULL;
	ret = 0;

	WT_RET(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "root:%s", btree->filename));
	WT_ERR(__wt_schema_table_read(session, key->data, vp));

err:	__wt_scr_free(&key);

	session->btree = btree;		/* XXX: schema-read overwrites */
	return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __btree_set_root --
 *	Set a non-schema file's root address.
 */
static int
__btree_set_root(WT_SESSION_IMPL *session, char *v)
{
	WT_BTREE *btree;
	WT_BUF *key;
	int ret;

	btree = session->btree;
	ret = 0;

	WT_RET(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "root:%s", btree->filename));
	WT_ERR(__wt_schema_table_update(session, key->data, v));

err:	__wt_scr_free(&key);

	session->btree = btree;		/* XXX: schema-insert overwrites */
	return (ret);
}
