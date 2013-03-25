/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_meta_turtle_init --
 *	Check the turtle file and create if necessary.
 */
int
__wt_meta_turtle_init(WT_SESSION_IMPL *session, int *existp)
{
	WT_DECL_RET;
	WT_ITEM *buf;
	int exist;
	const char *metaconf;
	const char *cfg[] = API_CONF_DEFAULTS(file, meta, NULL);

	buf = NULL;
	metaconf = NULL;
	*existp = 0;

	/* Discard any turtle setup file left-over from previous runs. */
	WT_RET(__wt_exist(session, WT_METADATA_TURTLE_SET, &exist));
	if (exist)
		WT_RET(__wt_remove(session, WT_METADATA_TURTLE_SET));

	/* If there's already a turtle file, we're done. */
	WT_RET(__wt_exist(session, WT_METADATA_TURTLE, &exist));
	if (exist) {
		*existp = 1;
		return (0);
	}

	/* Create a turtle file with default values. */
	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,
	    "key_format=S,value_format=S,version=(major=%d,minor=%d)",
	    WT_BTREE_MAJOR_VERSION, WT_BTREE_MINOR_VERSION));
	cfg[1] = buf->data;
	WT_ERR(__wt_config_collapse(session, cfg, &metaconf));
	WT_ERR(__wt_meta_turtle_update(session, WT_METADATA_URI, metaconf));

err:	__wt_free(session, metaconf);
	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __wt_meta_turtle_read --
 *	Read the turtle file.
 */
int
__wt_meta_turtle_read(
    WT_SESSION_IMPL *session, const char *key, const char **valuep)
{
	FILE *fp;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	int match;
	const char *path;

	*valuep = NULL;

	fp = NULL;
	path = NULL;

	/* Open the turtle file. */
	WT_RET(__wt_filename(session, WT_METADATA_TURTLE, &path));
	WT_ERR_TEST((fp = fopen(path, "r")) == NULL, __wt_errno());

	/* Search for the key. */
	WT_ERR(__wt_scr_alloc(session, 512, &buf));
	for (match = 0;;) {
		WT_ERR(__wt_getline(session, buf, fp));
		if (buf->size == 0)
			WT_ERR(WT_NOTFOUND);
		if (strcmp(key, buf->data) == 0)
			match = 1;

		/* Key matched: read the subsequent line for the value. */
		WT_ERR(__wt_getline(session, buf, fp));
		if (buf->size == 0)
			WT_ERR(__wt_illegal_value(session, WT_METADATA_TURTLE));
		if (match)
			break;
	}

	/* Copy the value for the caller. */
	WT_ERR(__wt_strdup(session, buf->data, valuep));

err:	if (fp != NULL)
		WT_TRET(fclose(fp) == 0 ? 0 : __wt_errno());
	if (path != NULL)
		__wt_free(session, path);
	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __wt_meta_turtle_update --
 *	Update the turtle file.
 */
int
__wt_meta_turtle_update(
    WT_SESSION_IMPL *session, const char *key,  const char *value)
{
	FILE *fp;
	WT_DECL_RET;
	WT_ITEM *buf;
	int vmajor, vminor, vpatch;
	const char *path, *version;

	buf = NULL;
	fp = NULL;

	version = wiredtiger_version(&vmajor, &vminor, &vpatch);

	/*
	 * Create the turtle setup file: we currently re-write it from scratch
	 * every time.
	 */
	WT_ERR(__wt_filename(session, WT_METADATA_TURTLE_SET, &path));
	WT_ERR_TEST((fp = fopen(path, "w")) == NULL, __wt_errno());

	WT_ERR_TEST((fprintf(fp,
	    "%s\n%s\n%s\n" "major=%d,minor=%d,patch=%d\n%s\n%s\n",
	    WT_METADATA_VERSION_STR, version,
	    WT_METADATA_VERSION, vmajor, vminor, vpatch,
	    key, value) < 0), __wt_errno());

	ret = fclose(fp);
	fp = NULL;
	WT_ERR_TEST(ret == EOF, __wt_errno());

	WT_ERR(
	    __wt_rename(session, WT_METADATA_TURTLE_SET, WT_METADATA_TURTLE));

	if (0) {
err:		WT_TRET(__wt_remove(session, WT_METADATA_TURTLE_SET));
	}

	if  (fp != NULL)
		WT_TRET(fclose(fp) == 0 ? 0 : __wt_errno());
	__wt_free(session, path);
	__wt_scr_free(&buf);

	return (ret);
}
