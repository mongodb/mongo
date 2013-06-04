/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int dump_config(WT_SESSION *, const char *, int);
static int dump_prefix(int);
static int dump_suffix(void);
static int dump_table_config(WT_SESSION *, WT_CURSOR *, const char *);
static int dump_table_config_type(WT_SESSION *,
    WT_CURSOR *, WT_CURSOR *, const char *, const char *, const char *);
static int print_config(WT_SESSION *, const char *, const char *, const char *);
static int usage(void);

static inline int
dump_forward(WT_CURSOR *cursor, const char *name)
{
	WT_DECL_RET;
	const char *key, *value;

	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr(name, "get_key", ret));
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(name, "get_value", ret));
		if (printf("%s\n%s\n", key, value) < 0)
			return (util_err(EIO, NULL));
	}
	return (ret == WT_NOTFOUND ? 0 : util_cerr(name, "next", ret));
}

static inline int
dump_reverse(WT_CURSOR *cursor, const char *name)
{
	WT_DECL_RET;
	const char *key, *value;

	while ((ret = cursor->prev(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr(name, "get_key", ret));
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(name, "get_value", ret));
		if (printf("%s\n%s\n", key, value) < 0)
			return (util_err(EIO, NULL));
	}
	return (ret == WT_NOTFOUND ? 0 : util_cerr(name, "prev", ret));
}

int
util_dump(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	size_t len;
	int ch, hex, reverse;
	char *checkpoint, *config, *name;

	hex = reverse = 0;
	checkpoint = config = name = NULL;
	while ((ch = util_getopt(argc, argv, "c:f:rx")) != EOF)
		switch (ch) {
		case 'c':
			checkpoint = util_optarg;
			break;
		case 'f':			/* output file */
			if (freopen(util_optarg, "w", stdout) == NULL)
				return (
				    util_err(errno, "%s: reopen", util_optarg));
			break;
		case 'r':
			reverse = 1;
			break;
		case 'x':
			hex = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* The remaining argument is the uri. */
	if (argc != 1)
		return (usage());
	if ((name = util_name(*argv,
	    "table", UTIL_FILE_OK | UTIL_LSM_OK | UTIL_TABLE_OK)) == NULL)
		goto err;

	if (dump_config(session, name, hex) != 0)
		goto err;

	len =
	    checkpoint == NULL ? 0 : strlen("checkpoint=") + strlen(checkpoint);
	len += strlen(hex ? "dump=hex" : "dump=print");
	if ((config = malloc(len + 10)) == NULL)
		goto err;
	if (checkpoint == NULL)
		config[0] = '\0';
	else {
		(void)strcpy(config, "checkpoint=");
		(void)strcat(config, checkpoint);
		(void)strcat(config, ",");
	}
	(void)strcat(config, hex ? "dump=hex" : "dump=print");
	if ((ret = session->open_cursor(
	    session, name, NULL, config, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}

	if (reverse)
		ret = dump_reverse(cursor, name);
	else
		ret = dump_forward(cursor, name);

	if (0) {
err:		ret = 1;
	}

	if (config != NULL)
		free(config);
	if (name != NULL)
		free(name);

	return (ret);
}

/*
 * dump_config --
 *	Dump the config for the uri.
 */
static int
dump_config(WT_SESSION *session, const char *uri, int hex)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_EXTENSION_API *wtext;
	int tret;
	const char *value;

	/* Dump the config. */
	if (WT_PREFIX_MATCH(uri, "table:")) {
		/* Open a metadata cursor. */
		if ((ret = session->open_cursor(
		    session, WT_METADATA_URI, NULL, NULL, &cursor)) != 0) {
			fprintf(stderr, "%s: %s: session.open_cursor: %s\n",
			    progname,
			    WT_METADATA_URI, wiredtiger_strerror(ret));
			return (1);
		}
		/*
		 * Search for the object itself, just to make sure it exists,
		 * we don't want to output a header if the user entered the
		 * wrong name.  This where we find out a table object doesn't
		 * exist, use a simple error message.
		 */
		cursor->set_key(cursor, uri);
		if ((ret = cursor->search(cursor)) == 0) {
			if (dump_prefix(hex) != 0 ||
			    dump_table_config(session, cursor, uri) != 0 ||
			    dump_suffix() != 0)
				ret = 1;
		} else if (ret == WT_NOTFOUND)
			ret = util_err(0, "%s: No such object exists", uri);
		else
			ret = util_err(ret, "%s", uri);

		if ((tret = cursor->close(cursor)) != 0) {
			tret = util_cerr(uri, "close", tret);
			if (ret == 0)
				ret = tret;
		}
	} else {
		/*
		 * We want to be able to dump the metadata file itself, but the
		 * configuration for that file lives in the turtle file.  Reach
		 * down into the library and ask for the file's configuration,
		 * that will work in all cases.
		 *
		 * This where we find out a file object doesn't exist, use a
		 * simple error message.
		 */
		wtext = session->
		    connection->get_extension_api(session->connection);
		if ((ret =
		    wtext->metadata_read(wtext, session, uri, &value)) == 0) {
			if (dump_prefix(hex) != 0 ||
			    print_config(session, uri, value, NULL) != 0 ||
			    dump_suffix() != 0)
				ret = 1;
		} else if (ret == WT_NOTFOUND)
			ret = util_err(0, "%s: No such object exists", uri);
		else
			ret = util_err(ret, "%s", uri);
	}

	return (ret);
}

/*
 * dump_table_config --
 *	Dump the config for a table.
 */
static int
dump_table_config(WT_SESSION *session, WT_CURSOR *cursor, const char *uri)
{
	WT_CURSOR *srch;
	WT_DECL_RET;
	int tret;
	const char *key, *name, *value;

	/* Get the table name. */
	if ((name = strchr(uri, ':')) == NULL) {
		fprintf(stderr, "%s: %s: corrupted uri\n", progname, uri);
		return (1);
	}
	++name;

	/*
	 * Dump out the config information: first, dump the uri entry itself
	 * (requires a lookup).
	 */
	cursor->set_key(cursor, uri);
	if ((ret = cursor->search(cursor)) != 0)
		return (util_cerr(uri, "search", ret));
	if ((ret = cursor->get_key(cursor, &key)) != 0)
		return (util_cerr(uri, "get_key", ret));
	if ((ret = cursor->get_value(cursor, &value)) != 0)
		return (util_cerr(uri, "get_value", ret));
	if (print_config(session, key, value, NULL) != 0)
		return (1);

	/*
	 * The underlying table configuration function needs a second cursor:
	 * open one before calling it, it makes error handling hugely simpler.
	 */
	if ((ret =
	    session->open_cursor(session, NULL, cursor, NULL, &srch)) != 0)
		return (util_cerr(uri, "open_cursor", ret));

	if ((ret = dump_table_config_type(
	    session, cursor, srch, uri, name, "colgroup:")) == 0)
		ret = dump_table_config_type(
		    session, cursor, srch, uri, name, "index:");

	if ((tret = srch->close(srch)) != 0) {
		tret = util_cerr(uri, "close", tret);
		if (ret == 0)
			ret = tret;
	}

	return (ret);
}

/*
 * dump_table_config_type --
 *	Dump the column groups or indices for a table.
 */
static int
dump_table_config_type(WT_SESSION *session,
    WT_CURSOR *cursor, WT_CURSOR *srch,
    const char *uri, const char *name, const char *entry)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	const char *key, *skip, *value, *value_source;
	int exact;
	char *p;

	/*
	 * Search the file looking for column group and index key/value pairs:
	 * for each one, look up the related source information and append it
	 * to the base record.
	 */
	cursor->set_key(cursor, entry);
	if ((ret = cursor->search_near(cursor, &exact)) != 0) {
		if (ret == WT_NOTFOUND)
			return (0);
		return (util_cerr(uri, "search_near", ret));
	}
	if (exact >= 0)
		goto match;
	while ((ret = cursor->next(cursor)) == 0) {
match:		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr(uri, "get_key", ret));

		/* Check if we've finished the list of entries. */
		if (!WT_PREFIX_MATCH(key, entry))
			return (0);

		/* Check for a table name match. */
		skip = key + strlen(entry);
		if (strncmp(
		    skip, name, strlen(name)) != 0 || skip[strlen(name)] != ':')
			continue;

		/* Get the value. */
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(uri, "get_value", ret));

		/* Crack it and get the underlying source. */
		if ((ret = __wt_config_getones(
		    (WT_SESSION_IMPL *)session, value, "source", &cval)) != 0)
			return (util_err(ret, "%s: source entry", key));

		/* Nul-terminate the source entry. */
		if ((p = malloc(cval.len + 10)) == NULL)
			return (util_err(errno, NULL));
		(void)strncpy(p, cval.str, cval.len);
		p[cval.len] = '\0';
		srch->set_key(srch, p);
		if ((ret = srch->search(srch)) != 0)
			ret = util_err(ret, "%s: %s", key, p);
		free(p);
		if (ret != 0)
			return (1);

		/* Get the source's value. */
		if ((ret = srch->get_value(srch, &value_source)) != 0)
			return (util_cerr(uri, "get_value", ret));

		/*
		 * The dumped configuration string is the original key plus the
		 * source's configuration.
		 */
		if (print_config(session, key, value, value_source) != 0)
			return (util_err(EIO, NULL));
	}
	if (ret == 0 || ret == WT_NOTFOUND)
		return (0);
	return (util_cerr(uri, "next", ret));
}

/*
 * dump_prefix --
 *	Output the dump file header prefix.
 */
static int
dump_prefix(int hex)
{
	int vmajor, vminor, vpatch;

	(void)wiredtiger_version(&vmajor, &vminor, &vpatch);

	if (printf(
	    "WiredTiger Dump (WiredTiger Version %d.%d.%d)\n",
	    vmajor, vminor, vpatch) < 0 ||
	    printf("Format=%s\n", hex ? "hex" : "print") < 0 ||
	    printf("Header\n") < 0)
		return (util_err(EIO, NULL));
	return (0);
}

/*
 * dump_suffix --
 *	Output the dump file header suffix.
 */
static int
dump_suffix(void)
{
	if (printf("Data\n") < 0)
		return (util_err(EIO, NULL));
	return (0);
}

/*
 * print_config --
 *	Output a key/value URI pair by combining v1 and v2.
 */
static int
print_config(WT_SESSION *session,
    const char *key, const char *v1, const char *v2)
{
	WT_DECL_RET;
	const char *value_ret;

	/*
	 * The underlying call will ignore v2 if v1 is NULL -- check here and
	 * swap in that case.
	 */
	if (v1 == NULL) {
		v1 = v2;
		v2 = NULL;
	}

	if ((ret = __wt_session_create_strip(session, v1, v2, &value_ret)) != 0)
		return (util_err(ret, NULL));
	ret = printf("%s\n%s\n", key, value_ret);
	free((char *)value_ret);
	if (ret < 0)
		return (util_err(EIO, NULL));
	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "dump [-rx] [-c checkpoint] [-f output-file] uri\n",
	    progname, usage_prefix);
	return (1);
}
