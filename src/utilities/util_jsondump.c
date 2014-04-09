/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int dump_begin(void);
static int dump_end(void);
static int dump_forward(WT_CURSOR *cursor, const char *name);
static int dump_reverse(WT_CURSOR *cursor, const char *name);
static int dump_separator(void);
static int dump_table_begin(WT_CURSOR *, const char *, const char *);
static int dump_table_cg(WT_CURSOR *, const char *, const char *, const char *,
    const char *);
static int dump_table_config(WT_SESSION *, const char *);
static int dump_table_end(void);
static int dup_json_string(const char *, char **);
static int usage(void);

/*
 * dump_begin --
 *	Output the dump file header prefix.
 */
static int
dump_begin(void)
{
	if (printf("{\n") < 0)
		return (util_err(EIO, NULL));
	return (0);
}

/*
 * dump_end --
 *	Output the dump file header suffix.
 */
static int
dump_end(void)
{
	if (printf("\n}\n") < 0)
		return (util_err(EIO, NULL));
	return (0);
}

static int
dump_forward(WT_CURSOR *cursor, const char *name)
{
	WT_DECL_RET;
	WT_ITEM key;
	WT_ITEM value;
	int once;

	once = 0;
	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr(name, "get_key", ret));
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(name, "get_value", ret));
		if (printf("%s\n            {\n%s,\n%s\n            }",
			(once ? "," : ""),
			(const char *)key.data,
			(const char *)value.data) < 0)
			return (util_err(EIO, NULL));
		once = 1;
	}
	if (once && printf("\n") < 0)
		return (util_err(EIO, NULL));
	return (ret == WT_NOTFOUND ? 0 : util_cerr(name, "next", ret));
}

static int
dump_reverse(WT_CURSOR *cursor, const char *name)
{
	WT_DECL_RET;
	WT_ITEM key;
	WT_ITEM value;
	int once;

	once = 0;
	while ((ret = cursor->prev(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr(name, "get_key", ret));
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(name, "get_value", ret));
		if (printf("%s            {\n%s,\n%s\n            }",
			(once ? ",\n" : ""),
			(const char *)key.data,
			(const char *)value.data) < 0)
			return (util_err(EIO, NULL));
		once = 1;
	}
	if (once && printf("\n") < 0)
		return (util_err(EIO, NULL));
	return (ret == WT_NOTFOUND ? 0 : util_cerr(name, "prev", ret));
}

/*
 * dump_begin --
 *	Output the dump file header prefix.
 */
static int
dump_separator(void)
{
	if (printf(",\n") < 0)
		return (util_err(EIO, NULL));
	return (0);
}

/*
 * dump_table_begin --
 *	Ouput the JSON syntax that starts a table, along with its config.
 */
static int
dump_table_begin(WT_CURSOR *cursor, const char *uri, const char *config)
{
	WT_DECL_RET;
	const char *name;
	char *jsonconfig;

	/* Get the table name. */
	if ((name = strchr(uri, ':')) == NULL) {
		fprintf(stderr, "%s: %s: corrupted uri\n", progname, uri);
		return (1);
	}
	++name;

	if ((ret = dup_json_string(config, &jsonconfig) != 0))
		return (util_cerr(uri, "config dup", ret));
	if (printf("    \"%s\" : [\n        {\n", uri) < 0)
		return (util_err(EIO, NULL));
	if (printf("            \"config\" : \"%s\",\n", jsonconfig) < 0)
		return (util_err(EIO, NULL));
	free(jsonconfig);

	if ((ret = dump_table_cg(
	    cursor, uri, name, "colgroup:", "colgroups")) == 0) {
		if (printf(",\n") < 0)
			ret = util_err(EIO, NULL);
		else
			ret = dump_table_cg(
			    cursor, uri, name, "index:", "indices");
	}

	if (printf("\n        },\n        [") < 0)
		return (util_err(EIO, NULL));

	return (ret);
}

/*
 * dump_table_cg --
 *	Dump the column groups or indices for a table.
 */
static int
dump_table_cg(WT_CURSOR *cursor,
    const char *uri, const char *name, const char *entry, const char *header)
{
	WT_DECL_RET;
	const char *key, *skip, *value;
	int exact, once;
	char *jsonconfig;
	static const char *indent = "                ";

	once = 0;
	if (printf("            \"%s\" : [", header) < 0)
		return (util_err(EIO, NULL));

	/*
	 * For table dumps, we're done.
	 */
	if (cursor == NULL) {
		if (printf("]") < 0)
			return (util_err(EIO, NULL));
		else
			return (0);
	}

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
			break;

		/* Check for a table name match. */
		skip = key + strlen(entry);
		if (strncmp(
		    skip, name, strlen(name)) != 0 || skip[strlen(name)] != ':')
			continue;

		/* Get the value. */
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(uri, "get_value", ret));

		if ((ret = dup_json_string(value, &jsonconfig) != 0))
			return (util_cerr(uri, "config dup", ret));
		ret = printf("%s\n"
		    "%s{\n"
		    "%s    \"uri\" : \"%s\",\n"
		    "%s    \"config\" : \"%s\"\n"
		    "%s}",
		    (once == 0 ? "" : ","),
		    indent, indent, key, indent, jsonconfig, indent);
		free(jsonconfig);
		if (ret < 0)
			return (util_err(EIO, NULL));

		once = 1;
	}
	if (printf("%s]", (once == 0 ? "" : "\n            ")) < 0)
		return (util_err(EIO, NULL));
	if (ret == 0 || ret == WT_NOTFOUND)
		return (0);
	return (util_cerr(uri, "next", ret));
}

/*
 * dump_table_config --
 *	Dump the config for the uri.
 */
static int
dump_table_config(WT_SESSION *session, const char *uri)
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
		    session, "metadata:", NULL, NULL, &cursor)) != 0) {
			fprintf(stderr, "%s: %s: session.open_cursor: %s\n",
			    progname, "metadata:", wiredtiger_strerror(ret));
			return (1);
		}

		/*
		 * Search for the object itself, to make sure it
		 * exists, and get its config string. This where we
		 * find out a table object doesn't exist, use a simple
		 * error message.
		 */
		cursor->set_key(cursor, uri);
		if ((ret = cursor->search(cursor)) == 0) {
			if ((ret = cursor->get_value(cursor, &value)) != 0)
				ret = util_cerr(uri, "get_value", ret);
			else if (dump_table_begin(cursor, uri,
			    value) != 0)
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
		    wtext->metadata_search(wtext, session, uri, &value)) == 0) {
			if (dump_table_begin(NULL, uri, value) != 0)
				ret = 1;
		} else if (ret == WT_NOTFOUND)
			ret = util_err(0, "%s: No such object exists", uri);
		else
			ret = util_err(ret, "%s", uri);
	}

	return (ret);
}

/*
 * dump_table_end --
 *	Output the JSON syntax that ends a table.
 */
static int
dump_table_end(void)
{
	if (printf("        ]\n    ]") < 0)
		return (util_err(EIO, NULL));
	return (0);
}

/*
 * dup_json_string --
 *	Like strdup, but escape any characters that are special for JSON.
 *	The result will be embedded in a JSON string.
 */
static int
dup_json_string(const char *str, char **result)
{
	size_t left, nchars;
	const char *p;
	char *q;

	nchars = 0;
	for (p = str; *p; p++, nchars++)
		nchars += __unpack_json_char(*p, NULL, 0, 0);
	q = malloc(nchars + 1);
	if (q == NULL)
		return 1;
	*result = q;
	left = nchars;
	for (p = str; *p; p++, nchars++) {
		nchars = __unpack_json_char(*p, q, left, 0);
		left -= nchars;
		q += nchars;
	}
	*q = '\0';
	return 0;
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "dump [-rx] [-f output-file] uri...\n",
	    progname, usage_prefix);
	return (1);
}

int
util_jsondump(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int ch, i, reverse;
	char *name;

	reverse = 0;
	name = NULL;
	while ((ch = util_getopt(argc, argv, "c:f:rx")) != EOF)
		switch (ch) {
		case 'f':			/* output file */
			if (freopen(util_optarg, "w", stdout) == NULL)
				return (
				    util_err(errno, "%s: reopen", util_optarg));
			break;
		case 'r':
			reverse = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* The remaining arguments are the uris. */
	if (argc < 1)
		return (usage());

	if ((ret = dump_begin()) != 0)
		goto err;

	for (i = 0; i < argc; i++) {
		if (i > 0) {
			free(name);
			if ((ret = dump_separator()) != 0)
				goto err;
		}
		if ((name = util_name(argv[i], "table",
		    UTIL_FILE_OK | UTIL_LSM_OK | UTIL_TABLE_OK)) == NULL)
			goto err;

		if (dump_table_config(session, name) != 0)
			goto err;

		if ((ret = session->open_cursor(
		    session, name, NULL, "json", &cursor)) != 0) {
			fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
			    progname, name, wiredtiger_strerror(ret));
			goto err;
		}

		if (reverse)
			ret = dump_reverse(cursor, name);
		else
			ret = dump_forward(cursor, name);
		if (ret != 0)
			goto err;

		if ((ret = dump_table_end()) != 0)
			goto err;
	}

	if ((ret = dump_end()) != 0)
		goto err;

	if (0) {
err:		ret = 1;
	}

	if (name != NULL)
		free(name);

	return (ret);
}
