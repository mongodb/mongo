/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __btree_get_root(WT_SESSION_IMPL *, const char **, int *, int *);
static int __btree_get_turtle(WT_SESSION_IMPL *, const char **, int *, int *);
static int __btree_set_root(WT_SESSION_IMPL *, char *);
static int __btree_set_turtle(WT_SESSION_IMPL *, char *);

#define	WT_TURTLE_MSG		"The turtle."

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
	int majorv, minorv, ret;
	const char *v;

	btree = session->btree;
	v = NULL;
	ret = 0;

	/* If there's no root address, return a NULL with a size of 0. */
	addr->data = NULL;
	addr->size = 0;

	/*
	 * If we don't find a file, we're creating a new one, at the current
	 * version.
	 */
	majorv = WT_BTREE_MAJOR_VERSION;
	minorv = WT_BTREE_MINOR_VERSION;

	/* Get the root address and major/minor numbers. */
	WT_ERR(strcmp(btree->filename, WT_SCHEMA_FILENAME) == 0 ?
	    __btree_get_turtle(session, &v, &majorv, &minorv) :
	    __btree_get_root(session, &v, &majorv, &minorv));

	if (majorv > WT_BTREE_MAJOR_VERSION ||
	    (majorv == WT_BTREE_MAJOR_VERSION &&
	    minorv > WT_BTREE_MINOR_VERSION))
		WT_ERR_MSG(session, EACCES,
		    "%s is an unsupported version of a WiredTiger file",
		    btree->filename);

	/* Nothing or "[NoAddr]" means no address. */
	if (v != NULL && strlen(v) != 0 && strcmp(v, WT_NOADDR) != 0) {
		WT_ERR(__wt_hex_to_raw(session, (void *)v, (void *)v, &size));
		WT_ERR(__wt_buf_set(session, addr, v, size));
	}

err:	if (ret != 0)
		__wt_errx(session,
		    "unable to find %s file's root address", btree->filename);

	__wt_free(session, v);
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
	    __btree_set_turtle(session, (char *)v->data) :
	    __btree_set_root(session, (char *)v->data));

err:	if (ret != 0)
		__wt_errx(session,
		    "unable to update %s file's root address", btree->filename);

	__wt_scr_free(&v);
	return (ret);
}

/*
 * __btree_get_turtle --
 *	Get the schema file's root address.
 */
static int
__btree_get_turtle(
    WT_SESSION_IMPL *session, const char **vp, int *majorp, int *minorp)
{
	FILE *fp;
	int found_root, found_version, ret;
	const char *path;
	char *p, line[1024];

	*vp = NULL;

	fp = NULL;
	ret = 0;
	path = NULL;

	found_root = found_version = 0;

	WT_ERR(__wt_filename(session, WT_SCHEMA_TURTLE, &path));
	WT_ERR_TEST((fp = fopen(path, "r")) == NULL, 0);
	for (;;) {
		if (fgets(line, (int)sizeof(line), fp) == NULL) {
			if (ferror(fp))  {
				ret = errno;
				goto format;
			}
			break;
		}
		if ((p = strchr(line, '\n')) == NULL)
			goto format;
		*p = '\0';

		if (strcmp(line, WT_TURTLE_MSG) == 0)
			continue;
		if (strncmp(line, "root:", strlen("root:")) == 0) {
			WT_ERR(__wt_strdup(
			    session, line + strlen("root:"), vp));
			found_root = 1;
			continue;
		}
		if (strncmp(line, "version:", strlen("version:")) == 0) {
			if (sscanf(line,
			    "version:major=%d,minor=%d", majorp, minorp) != 2)
				goto format;
			found_version = 1;
			continue;
		}
		goto format;
	}

	if (!found_root || !found_version) {
format:		__wt_errx(session, "the %s file is corrupted", path);
		ret = __wt_illegal_value(session);
	}

err:	if (fp != NULL)
		WT_TRET(fclose(fp));
	__wt_free(session, path);

	return (ret);
}

/*
 * __btree_set_turtle --
 *	Set the schema file's root address.
 */
static int
__btree_set_turtle(WT_SESSION_IMPL *session, char *v)
{
	WT_BUF *buf;
	FILE *fp;
	size_t len;
	int ret;
	const char *path;

	buf = NULL;
	ret = 0;
	path = NULL;

	WT_ERR(__wt_filename(session, WT_SCHEMA_TURTLE_SET, &path));
	WT_ERR_TEST((fp = fopen(path, "w")) == NULL, WT_ERROR);

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,
	    "%s\n"
	    "root:%s\n"
	    "version:major=%d,minor=%d\n",
	    WT_TURTLE_MSG, v, WT_BTREE_MAJOR_VERSION, WT_BTREE_MINOR_VERSION));
	len = (size_t)fprintf(fp, "%s", (char *)buf->data);
	if (len != buf->size)
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
	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __btree_get_root --
 *	Get a non-schema file's root address.
 */
static int
__btree_get_root(
    WT_SESSION_IMPL *session, const char **vp, int *majorp, int *minorp)
{
	WT_BTREE *btree;
	WT_BUF *key;
	int ret;
	const char *version;

	*vp = NULL;

	btree = session->btree;
	key = NULL;
	ret = 0;
	version = NULL;

	WT_RET(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "root:%s", btree->filename));
	WT_ERR(__wt_schema_table_read(session, key->data, vp));

	WT_ERR(__wt_buf_fmt(session, key, "version:%s", btree->filename));
	WT_ERR(__wt_schema_table_read(session, key->data, &version));
	if (sscanf(version, "major=%d,minor=%d", majorp, minorp) != 2)
		WT_ERR_MSG(session, EINVAL,
		    "unable to find %s file's version number", btree->filename);

	__wt_free(session, version);
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
	key = NULL;
	ret = 0;

	WT_RET(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "root:%s", btree->filename));
	WT_ERR(__wt_schema_table_update(session, key->data, v));

err:	__wt_scr_free(&key);

	session->btree = btree;		/* XXX: schema-insert overwrites */
	return (ret);
}
