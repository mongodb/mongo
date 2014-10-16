/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int dump_config(WT_SESSION *, const char *, int);
static int dump_json_begin(void);
static int dump_json_end(void);
static int dump_json_separator(void);
static int dump_json_table_begin(
    WT_SESSION *, WT_CURSOR *, const char *, const char *);
static int dump_json_table_cg(WT_SESSION *, WT_CURSOR *,
    const char *, const char *, const char *, const char *);
static int dump_json_table_config(WT_SESSION *, const char *);
static int dump_json_table_end(void);
static int dump_prefix(int);
static int dump_record(WT_CURSOR *, const char *, int, int);
static int dump_suffix(void);
static int dump_table_config(WT_SESSION *, WT_CURSOR *, const char *);
static int dump_table_config_type(WT_SESSION *,
    WT_CURSOR *, WT_CURSOR *, const char *, const char *, const char *);
static int dup_json_string(const char *, char **);
static int print_config(WT_SESSION *, const char *, const char *, const char *);
static int usage(void);

int
util_dump(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	size_t len;
	int ch, hex, i, json, reverse;
	char *checkpoint, *config, *name;

	hex = json = reverse = 0;
	checkpoint = config = name = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, "c:f:jrx")) != EOF)
		switch (ch) {
		case 'c':
			checkpoint = __wt_optarg;
			break;
		case 'f':			/* output file */
			if (freopen(__wt_optarg, "w", stdout) == NULL)
				return (
				    util_err(errno, "%s: reopen", __wt_optarg));
			break;
		case 'j':
			json = 1;
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
	argc -= __wt_optind;
	argv += __wt_optind;

	/* -j and -x are incompatible. */
	if (hex && json) {
		fprintf(stderr,
		    "%s: the -j and -x dump options are incompatible\n",
		    progname);
		goto err;
	}

	/* The remaining argument is the uri. */
	if (argc < 1 || (argc != 1 && !json))
		return (usage());

	if (json && (ret = dump_json_begin()) != 0)
		goto err;

	for (i = 0; i < argc; i++) {
		if (json && i > 0)
			if ((ret = dump_json_separator()) != 0)
				goto err;
		if (name != NULL) {
			free(name);
			name = NULL;
		}
		if ((name = util_name(argv[i], "table")) == NULL)
			goto err;

		if (json && dump_json_table_config(session, name) != 0)
			goto err;
		if (!json && dump_config(session, name, hex) != 0)
			goto err;

		len =
		    checkpoint == NULL ? 0 : strlen("checkpoint=") +
		    strlen(checkpoint) + 1;
		len += strlen(json ? "dump=json" :
		    (hex ? "dump=hex" : "dump=print"));
		if ((config = malloc(len + 10)) == NULL)
			goto err;
		if (checkpoint == NULL)
			config[0] = '\0';
		else {
			(void)strcpy(config, "checkpoint=");
			(void)strcat(config, checkpoint);
			(void)strcat(config, ",");
		}
		(void)strcat(config, json ? "dump=json" :
		    (hex ? "dump=hex" : "dump=print"));
		if ((ret = session->open_cursor(
		    session, name, NULL, config, &cursor)) != 0) {
			fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
			    progname, name, wiredtiger_strerror(ret));
			goto err;
		}

		if ((ret = dump_record(cursor, name, reverse, json)) != 0)
			goto err;
		if (json && (ret = dump_json_table_end()) != 0)
			goto err;
	}
	if (json && ((ret = dump_json_end()) != 0))
		goto err;

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
	int tret;

	/* Open a metadata cursor. */
	if ((ret = session->open_cursor(
	    session, WT_METADATA_URI, NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: %s: session.open_cursor: %s\n",
		    progname, WT_METADATA_URI, wiredtiger_strerror(ret));
		return (1);
	}
	/*
	 * Search for the object itself, just to make sure it exists, we don't
	 * want to output a header if the user entered the wrong name. This is
	 * where we find out a table doesn't exist, use a simple error message.
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

	return (ret);
}

/*
 * dump_json_begin --
 *	Output the dump file header prefix.
 */
static int
dump_json_begin(void)
{
	if (printf("{\n") < 0)
		return (util_err(EIO, NULL));
	return (0);
}

/*
 * dump_json_end --
 *	Output the dump file header suffix.
 */
static int
dump_json_end(void)
{
	if (printf("\n}\n") < 0)
		return (util_err(EIO, NULL));
	return (0);
}

/*
 * dump_json_begin --
 *	Output the dump file header prefix.
 */
static int
dump_json_separator(void)
{
	if (printf(",\n") < 0)
		return (util_err(EIO, NULL));
	return (0);
}

/*
 * dump_json_table_begin --
 *	Output the JSON syntax that starts a table, along with its config.
 */
static int
dump_json_table_begin(
    WT_SESSION *session, WT_CURSOR *cursor, const char *uri, const char *config)
{
	WT_DECL_RET;
	const char *name;
	char *jsonconfig, *stripped;

	jsonconfig = NULL;

	/* Get the table name. */
	if ((name = strchr(uri, ':')) == NULL) {
		fprintf(stderr, "%s: %s: corrupted uri\n", progname, uri);
		return (1);
	}
	++name;

	if ((ret = 
	    __wt_session_create_strip(session, config, NULL, &stripped)) != 0)
		return (util_err(ret, NULL));
	ret = dup_json_string(stripped, &jsonconfig);
	free(stripped);
	if (ret != 0)
		return (util_cerr(uri, "config dup", ret));
	if (printf("    \"%s\" : [\n        {\n", uri) < 0)
		goto eio;
	if (printf("            \"config\" : \"%s\",\n", jsonconfig) < 0)
		goto eio;

	if ((ret = dump_json_table_cg(
	    session, cursor, uri, name, "colgroup:", "colgroups")) == 0) {
		if (printf(",\n") < 0)
			goto eio;
		ret = dump_json_table_cg(
		    session, cursor, uri, name, "index:", "indices");
	}

	if (printf("\n        },\n        {\n            \"data\" : [") < 0)
		goto eio;

	if (0) {
eio:		ret = util_err(EIO, NULL);
	}

	free(jsonconfig);
	return (ret);
}

/*
 * dump_json_table_cg --
 *	Dump the column groups or indices for a table.
 */
static int
dump_json_table_cg(WT_SESSION *session, WT_CURSOR *cursor,
    const char *uri, const char *name, const char *entry, const char *header)
{
	WT_DECL_RET;
	const char *key, *skip, *value;
	int exact, once;
	char *jsonconfig, *stripped;
	static const char * const indent = "                ";

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

		if ((ret = __wt_session_create_strip(
		    session, value, NULL, &stripped)) != 0)
			return (util_err(ret, NULL));
		ret = dup_json_string(stripped, &jsonconfig);
		free(stripped);
		if (ret != 0)
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
 * dump_json_table_config --
 *	Dump the config for the uri.
 */
static int
dump_json_table_config(WT_SESSION *session, const char *uri)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_EXTENSION_API *wtext;
	int tret;
	char *value;

	/* Dump the config. */
	if (WT_PREFIX_MATCH(uri, "table:")) {
		/* Open a metadata cursor. */
		if ((ret = session->open_cursor(
		    session, WT_METADATA_URI, NULL, NULL, &cursor)) != 0) {
			fprintf(stderr, "%s: %s: session.open_cursor: %s\n",
			    progname, WT_METADATA_URI,
			    wiredtiger_strerror(ret));
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
			else if (dump_json_table_begin(
			    session, cursor, uri, value) != 0)
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
			if (dump_json_table_begin(
			    session, NULL, uri, value) != 0)
				ret = 1;
		} else if (ret == WT_NOTFOUND)
			ret = util_err(0, "%s: No such object exists", uri);
		else
			ret = util_err(ret, "%s", uri);
	}

	return (ret);
}

/*
 * dump_json_table_end --
 *	Output the JSON syntax that ends a table.
 */
static int
dump_json_table_end(void)
{
	if (printf("            ]\n        }\n    ]") < 0)
		return (util_err(EIO, NULL));
	return (0);
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
 * dump_record --
 *	Dump a single record, advance cursor to next/prev, along
 *	with JSON formatting if needed.
 */
static int
dump_record(WT_CURSOR *cursor, const char *name, int reverse, int json)
{
	WT_DECL_RET;
	const char *infix, *key, *prefix, *suffix, *value;
	int once;

	once = 0;
	if (json) {
		prefix = "\n{\n";
		infix = ",\n";
		suffix = "\n}";
	} else {
		prefix = "";
		infix = "\n";
		suffix = "\n";
	}
	while ((ret =
	    (reverse ? cursor->prev(cursor) : cursor->next(cursor))) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr(name, "get_key", ret));
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(name, "get_value", ret));
		if (printf("%s%s%s%s%s%s", (json && once) ? "," : "",
		    prefix, key, infix, value, suffix) < 0)
			return (util_err(EIO, NULL));
		once = 1;
	}
	if (json && once && printf("\n") < 0)
		return (util_err(EIO, NULL));
	return (ret == WT_NOTFOUND ? 0 :
	    util_cerr(name, (reverse ? "prev" : "next"), ret));
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
		nchars += __wt_json_unpack_char(*p, NULL, 0, 0);
	q = malloc(nchars + 1);
	if (q == NULL)
		return (1);
	*result = q;
	left = nchars;
	for (p = str; *p; p++, nchars++) {
		nchars = __wt_json_unpack_char(*p, (u_char *)q, left, 0);
		left -= nchars;
		q += nchars;
	}
	*q = '\0';
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
	char *value_ret;

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
	    "dump [-jrx] [-c checkpoint] [-f output-file] uri\n",
	    progname, usage_prefix);
	return (1);
}
