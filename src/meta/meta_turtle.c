/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_turtle_init --
 *	Check on the turtle file.
 */
int
__wt_turtle_init(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_ITEM *buf;
	int exist;
	const char *metaconf, *path;
	const char *cfg[] = API_CONF_DEFAULTS(file, meta, NULL);

	buf = NULL;
	metaconf = path = NULL;

	/* Discard any turtle setup file left-over from previous runs. */
	WT_RET(__wt_exist(session, WT_METADATA_TURTLE_SET, &exist));
	if (exist)
		WT_RET(__wt_remove(session, WT_METADATA_TURTLE_SET));

	/* If there's already a turtle file, we're done. */
	WT_RET(__wt_exist(session, WT_METADATA_TURTLE, &exist));
	if (exist)
		return (0);

	/* Create a turtle file with default values. */
	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,
	    "key_format=S,value_format=S,version=(major=%d,minor=%d)",
	    WT_BTREE_MAJOR_VERSION, WT_BTREE_MINOR_VERSION));
	cfg[1] = buf->data;
	WT_RET(__wt_config_collapse(session, cfg, &metaconf));
	WT_ERR(__wt_turtle_update(session, WT_METADATA_URI, metaconf));

err:	__wt_free(session, metaconf);
	__wt_free(session, path);
	__wt_scr_free(&buf);

	return (ret);
}

/*
 * __wt_turtle_read --
 *	Read the turtle file.
 */
int
__wt_turtle_read(WT_SESSION_IMPL *session, const char *key, const char **valuep)
{
	FILE *fp;
	WT_DECL_RET;
	const char *path;
	char *p, line[1024];

	fp = NULL;
	path = NULL;

	/* Open the turtle file. */
	WT_RET(__wt_filename(session, WT_METADATA_TURTLE, &path));
	WT_ERR_TEST((fp = fopen(path, "r")) == NULL, WT_NOTFOUND);

	/* Search for the key. */
	ret = WT_NOTFOUND;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if ((p = strchr(line, '\n')) == NULL)
			goto format;
		*p = '\0';
		if (strcmp(key, line) == 0)
			ret = 0;

		/* Key matched: read the subsequent line for the value. */
		if (fgets(line, sizeof(line), fp) == NULL)
			goto format;
		if ((p = strchr(line, '\n')) == NULL)
			goto format;
		*p = '\0';
		if (ret == 0)
			break;
	}

	/* Check for an I/O error. */
	if (ferror(fp))
		WT_ERR(__wt_errno());
	WT_ERR(ret);

	/* Successful: copy the value for the caller. */
	WT_ERR(__wt_strdup(session, line, valuep));

	if (0) {
format:		return (__wt_illegal_value(session, WT_METADATA_TURTLE));
	}

err:	if (fp != NULL)
		WT_TRET(fclose(fp));
	__wt_free(session, path);
	return (ret);
}

/*
 * __wt_turtle_update --
 *	Update the turtle file.
 */
int
__wt_turtle_update(
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

	WT_ERR_TEST(fclose(fp) == EOF, __wt_errno());
	fp = NULL;

	ret = __wt_rename(session, WT_METADATA_TURTLE_SET, WT_METADATA_TURTLE);

	if (0) {
err:		(void)__wt_remove(session, WT_METADATA_TURTLE_SET);
	}

	if  (fp != NULL)
		(void)fclose(fp);
	__wt_free(session, path);
	__wt_scr_free(&buf);

	return (ret);
}
