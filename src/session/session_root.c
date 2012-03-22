/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __btree_get_root(
	WT_SESSION_IMPL *, const char *, const char **, int *, int *);
static int __btree_get_turtle(WT_SESSION_IMPL *, const char **, int *, int *);
static int __btree_parse_root(
	WT_SESSION_IMPL *, const char *, const char **, int *, int *);
static int __btree_set_root(WT_SESSION_IMPL *, const char *, WT_ITEM *);
static int __btree_set_turtle(WT_SESSION_IMPL *, WT_ITEM *);

#define	WT_TURTLE_MSG		"The turtle."

#define	WT_SCHEMA_TURTLE	"WiredTiger.turtle"	/* Schema root page */
#define	WT_SCHEMA_TURTLE_SET	"WiredTiger.turtle.set"	/* Schema root prep */

/*
 * __wt_btree_get_root --
 *	Get the file's root address.
 */
int
__wt_btree_get_root(WT_SESSION_IMPL *session, WT_ITEM *addr)
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
	    __btree_get_root(session, btree->filename, &v, &majorv, &minorv));

	if (majorv > WT_BTREE_MAJOR_VERSION ||
	    (majorv == WT_BTREE_MAJOR_VERSION &&
	    minorv > WT_BTREE_MINOR_VERSION))
		WT_ERR_MSG(session, EACCES,
		    "%s is an unsupported version of a WiredTiger file",
		    btree->filename);

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
 * __wt_btree_set_root --
 *	Set the file's root address.
 */
int
__wt_btree_set_root(WT_SESSION_IMPL *session,
    const char *filename, uint8_t *addr, uint32_t size)
{
	WT_ITEM *v;
	int ret;

	v = NULL;
	ret = 0;

	/*
	 * Every bytes is encoded as 2 bytes, plus a trailing nul byte,
	 * and it needs to hold the [NoAddr] string.
	 */
	WT_RET(__wt_scr_alloc(
	    session, size * 2 + 1 + WT_STORE_SIZE(strlen(WT_NOADDR)), &v));

	/*
	 * We're not using the WT_ITEM as a buffer going forward, but fill
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

	WT_ERR(strcmp(filename, WT_SCHEMA_FILENAME) == 0 ?
	    __btree_set_turtle(session, v) :
	    __btree_set_root(session, filename, v));

err:	/*
	 * If we are unrolling a failed create, we may have already removed
	 * the schema table entry.  If no entry is found to update and we're
	 * trying to clear the root, just ignore it.
	 */
	if (ret == WT_NOTFOUND && addr == NULL)
		ret = 0;
	if (ret != 0)
		__wt_errx(session,
		    "unable to update %s file's root address", filename);

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
	int ret;
	const char *path;
	char line[1024], *p;

	*vp = NULL;

	fp = NULL;
	ret = 0;
	path = NULL;

	WT_RET(__wt_filename(session, WT_SCHEMA_TURTLE, &path));
	if ((fp = fopen(path, "r")) == NULL)
		goto done;
	while (fgets(line, (int)sizeof(line), fp) != NULL) {
		if ((p = strchr(line, '\n')) == NULL)
			break;
		*p = '\0';
		if (strcmp(line, WT_TURTLE_MSG) == 0)
			continue;

		WT_ERR(__btree_parse_root(session, line, vp, majorp, minorp));
		goto done;
	}

	if (ferror(fp))
		ret = __wt_errno();
err:	if (ret == 0)
		ret = WT_ERROR;
	__wt_errx(session, "the %s file is corrupted", path);

done:	if (fp != NULL)
		WT_TRET(fclose(fp));
	__wt_free(session, path);

	return (ret);
}

/*
 * __btree_set_turtle --
 *	Set the schema file's root address.
 */
static int
__btree_set_turtle(WT_SESSION_IMPL *session, WT_ITEM *v)
{
	WT_ITEM *buf;
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
	    "root=%.*s,version=(major=%d,minor=%d)\n", WT_TURTLE_MSG,
	    (int)v->size, (const char *)v->data,
	    WT_BTREE_MAJOR_VERSION, WT_BTREE_MINOR_VERSION));
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
 * __btree_parse_root --
 *	Parse a btree config string to extract the version
 */
static int
__btree_parse_root(WT_SESSION_IMPL *session,
    const char *config, const char **vp, int *majorp, int *minorp)
{
	WT_CONFIG_ITEM subv, v;

	*vp = NULL;
	*majorp = *minorp = 0;

	WT_RET(__wt_config_getones(session, config, "version", &v));
	WT_RET(__wt_config_subgets(session, &v, "major", &subv));
	*majorp = (int)subv.val;

	WT_RET(__wt_config_subgets(session, &v, "minor", &subv));
	*minorp = (int)subv.val;

	WT_RET(__wt_config_getones(session, config, "root", &v));
	if (v.len > 0)
		WT_RET(__wt_strndup(session, v.str, v.len, vp));

	return (0);
}

/*
 * __btree_get_root --
 *	Parse a btree config string to extract the version
 */
static int
__btree_get_root(WT_SESSION_IMPL *session,
    const char *filename, const char **vp, int *majorp, int *minorp)
{
	WT_ITEM *key;
	const char *config;
	int ret;

	config = NULL;
	key = NULL;
	*vp = NULL;
	*majorp = *minorp = 0;

	WT_ERR(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "file:%s", filename));
	WT_ERR(__wt_schema_table_read(session, key->data, &config));
	WT_ERR(__btree_parse_root(session, config, vp, majorp, minorp));

err:	__wt_scr_free(&key);
	__wt_free(session, config);
	return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __btree_set_root --
 *	Set a non-schema file's root address.
 */
static int
__btree_set_root(WT_SESSION_IMPL *session, const char *filename, WT_ITEM *v)
{
	WT_ITEM *key, *newv;
	const char *cfg[3], *newcfg;
	int ret;

	key = newv = NULL;
	cfg[0] = newcfg = NULL;

	WT_ERR(__wt_scr_alloc(session, 0, &key));
	WT_ERR(__wt_buf_fmt(session, key, "file:%s", filename));
	WT_ERR(__wt_schema_table_read(session, key->data, &cfg[0]));
	WT_ERR(__wt_scr_alloc(session, 0, &newv));
	WT_ERR(__wt_buf_fmt(session, newv, "root=%.*s",
	    (int)v->size, (const char *)v->data));
	cfg[1] = newv->data;
	cfg[2] = NULL;
	WT_ERR(__wt_config_collapse(session, cfg, &newcfg));
	WT_ERR(__wt_schema_table_update(session, key->data, newcfg));

err:	__wt_scr_free(&key);
	__wt_scr_free(&newv);
	__wt_free(session, cfg[0]);
	__wt_free(session, newcfg);
	return (ret);
}
