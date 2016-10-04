/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"
#include "util_dump.h"

static int dump_config(WT_SESSION *, const char *, bool, bool);
static int dump_json_begin(WT_SESSION *);
static int dump_json_end(WT_SESSION *);
static int dump_json_separator(WT_SESSION *);
static int dump_json_table_end(WT_SESSION *);
static int dump_prefix(WT_SESSION *, bool, bool);
static int dump_record(WT_CURSOR *, bool, bool);
static int dump_suffix(WT_SESSION *, bool);
static int dump_table_config(WT_SESSION *, WT_CURSOR *, const char *, bool);
static int dump_table_parts_config(
    WT_SESSION *, WT_CURSOR *, const char *, const char *, bool);
static int dup_json_string(const char *, char **);
static int print_config(WT_SESSION *, const char *, const char *, bool, bool);
static int usage(void);

int
util_dump(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	size_t len;
	int ch, i;
	bool hex, json, reverse;
	char *checkpoint, *config, *name;

	hex = json = reverse = false;
	checkpoint = config = name = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, "c:f:jrx")) != EOF)
		switch (ch) {
		case 'c':
			checkpoint = __wt_optarg;
			break;
		case 'f':			/* output file */
			if (freopen(__wt_optarg, "w", stdout) == NULL)
				return (util_err(
				    session, errno, "%s: reopen", __wt_optarg));
			break;
		case 'j':
			json = true;
			break;
		case 'r':
			reverse = true;
			break;
		case 'x':
			hex = true;
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

	if (json &&
	    ((ret = dump_json_begin(session)) != 0 ||
	    (ret = dump_prefix(session, hex, json)) != 0))
		goto err;

	for (i = 0; i < argc; i++) {
		if (json && i > 0)
			if ((ret = dump_json_separator(session)) != 0)
				goto err;
		free(name);
		name = NULL;

		if ((name = util_name(session, argv[i], "table")) == NULL)
			goto err;

		if (dump_config(session, name, hex, json) != 0)
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
			    progname, name, session->strerror(session, ret));
			goto err;
		}

		if ((ret = dump_record(cursor, reverse, json)) != 0)
			goto err;
		if (json && (ret = dump_json_table_end(session)) != 0)
			goto err;
	}
	if (json && ((ret = dump_json_end(session)) != 0))
		goto err;

	if (0) {
err:		ret = 1;
	}

	free(config);
	free(name);

	return (ret);
}

/*
 * dump_config --
 *	Dump the config for the uri.
 */
static int
dump_config(WT_SESSION *session, const char *uri, bool hex, bool json)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int tret;

	/* Open a metadata cursor. */
	if ((ret = session->open_cursor(
	    session, "metadata:create", NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: %s: session.open_cursor: %s\n", progname,
		    "metadata:create", session->strerror(session, ret));
		return (1);
	}
	/*
	 * Search for the object itself, just to make sure it exists, we don't
	 * want to output a header if the user entered the wrong name. This is
	 * where we find out a table doesn't exist, use a simple error message.
	 */
	cursor->set_key(cursor, uri);
	if ((ret = cursor->search(cursor)) == 0) {
		if ((!json && dump_prefix(session, hex, json) != 0) ||
		    dump_table_config(session, cursor, uri, json) != 0 ||
		    dump_suffix(session, json) != 0)
			ret = 1;
	} else if (ret == WT_NOTFOUND)
		ret = util_err(session, 0, "%s: No such object exists", uri);
	else
		ret = util_err(session, ret, "%s", uri);

	if ((tret = cursor->close(cursor)) != 0) {
		tret = util_cerr(cursor, "close", tret);
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
dump_json_begin(WT_SESSION *session)
{
	if (printf("{\n") < 0)
		return (util_err(session, EIO, NULL));
	return (0);
}

/*
 * dump_json_end --
 *	Output the dump file header suffix.
 */
static int
dump_json_end(WT_SESSION *session)
{
	if (printf("\n}\n") < 0)
		return (util_err(session, EIO, NULL));
	return (0);
}

/*
 * dump_json_begin --
 *	Output a separator between two JSON outputs in a list.
 */
static int
dump_json_separator(WT_SESSION *session)
{
	if (printf(",\n") < 0)
		return (util_err(session, EIO, NULL));
	return (0);
}

/*
 * dump_json_table_end --
 *	Output the JSON syntax that ends a table.
 */
static int
dump_json_table_end(WT_SESSION *session)
{
	if (printf("            ]\n        }\n    ]") < 0)
		return (util_err(session, EIO, NULL));
	return (0);
}

/*
 * dump_table_config --
 *	Dump the config for a table.
 */
static int
dump_table_config(
    WT_SESSION *session, WT_CURSOR *cursor, const char *uri, bool json)
{
	WT_DECL_RET;
	const char *name, *v;

	/* Get the table name. */
	if ((name = strchr(uri, ':')) == NULL) {
		fprintf(stderr, "%s: %s: corrupted uri\n", progname, uri);
		return (1);
	}
	++name;

	/*
	 * Dump out the config information: first, dump the uri entry itself,
	 * it overrides all subsequent configurations.
	 */
	cursor->set_key(cursor, uri);
	if ((ret = cursor->search(cursor)) != 0)
		return (util_cerr(cursor, "search", ret));
	if ((ret = cursor->get_value(cursor, &v)) != 0)
		return (util_cerr(cursor, "get_value", ret));

	WT_RET(print_config(session, uri, v, json, true));

	WT_RET(dump_table_parts_config(
	    session, cursor, name, "colgroup:", json));
	WT_RET(dump_table_parts_config(
	    session, cursor, name, "index:", json));

	return (0);
}

/*
 * dump_table_parts_config --
 *	Dump the column groups or indices parts with a table.
 */
static int
dump_table_parts_config(WT_SESSION *session, WT_CURSOR *cursor,
    const char *name, const char *entry, bool json)
{
	WT_DECL_RET;
	bool multiple;
	const char *groupname, *key, *sep;
	size_t len;
	int exact;
	const char *v;
	char *uriprefix;

	multiple = false;
	sep = "";
	uriprefix = NULL;

	if (json) {
		if (strcmp(entry, "colgroup:") == 0) {
			groupname = "colgroups";
			sep = ",";
		} else {
			groupname = "indices";
		}
		if (printf("            \"%s\" : [", groupname) < 0)
			return (util_err(session, EIO, NULL));
	}

	len = strlen(entry) + strlen(name) + 1;
	if ((uriprefix = malloc(len)) == NULL)
		return util_err(session, errno, NULL);

	snprintf(uriprefix, len, "%s%s", entry, name);

	/*
	 * Search the file looking for column group and index key/value pairs:
	 * for each one, look up the related source information and append it
	 * to the base record, where the column group and index configuration
	 * overrides the source configuration.
	 */
	cursor->set_key(cursor, uriprefix);
	ret = cursor->search_near(cursor, &exact);
	free(uriprefix);
	if (ret == WT_NOTFOUND)
		return (0);
	if (ret != 0)
		return (util_cerr(cursor, "search_near", ret));

	/*
	 * An exact match is only possible for column groups, and indicates
	 * there is an implicit (unnamed) column group.  Any configuration
	 * for such a column group has already been folded into the
	 * configuration for the associated table, so it is not interesting.
	 */
	if (exact > 0)
		goto match;
	while (exact != 0 && (ret = cursor->next(cursor)) == 0) {
match:		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr(cursor, "get_key", ret));

		/* Check if we've finished the list of entries. */
		if (!WT_PREFIX_MATCH(key, entry) ||
		    !WT_PREFIX_MATCH(key + strlen(entry), name))
			break;

		if ((ret = cursor->get_value(cursor, &v)) != 0)
			return (util_cerr(cursor, "get_value", ret));

		if (json && printf("%s\n", (multiple ? "," : "")) < 0)
			return (util_err(session, EIO, NULL));
		/*
		 * The dumped configuration string is the original key plus the
		 * source's configuration, where the values of the original key
		 * override any source configurations of the same name.
		 */
		if (print_config(session, key, v, json, false) != 0)
			return (util_err(session, EIO, NULL));
		multiple = true;
	}
	if (json && printf("%s]%s\n",
	    (multiple ? "\n            " : ""), sep) < 0)
		return (util_err(session, EIO, NULL));

	if (ret == 0 || ret == WT_NOTFOUND)
		return (0);
	return (util_cerr(cursor, "next", ret));
}

/*
 * dump_prefix --
 *	Output the dump file header prefix.
 */
static int
dump_prefix(WT_SESSION *session, bool hex, bool json)
{
	int vmajor, vminor, vpatch;

	(void)wiredtiger_version(&vmajor, &vminor, &vpatch);

	if (!json && (printf(
	    "WiredTiger Dump (WiredTiger Version %d.%d.%d)\n",
	    vmajor, vminor, vpatch) < 0 ||
	    printf("Format=%s\n", hex ? "hex" : "print") < 0 ||
	    printf("Header\n") < 0))
		return (util_err(session, EIO, NULL));
	else if (json && printf(
	    "    \"%s\" : \"%d (%d.%d.%d)\",\n",
	    DUMP_JSON_VERSION_MARKER, DUMP_JSON_CURRENT_VERSION,
	    vmajor, vminor, vpatch) < 0)
		return (util_err(session, EIO, NULL));

	return (0);
}

/*
 * dump_record --
 *	Dump a single record, advance cursor to next/prev, along
 *	with JSON formatting if needed.
 */
static int
dump_record(WT_CURSOR *cursor, bool reverse, bool json)
{
	WT_DECL_RET;
	WT_SESSION *session;
	const char *infix, *key, *prefix, *suffix, *value;
	bool once;

	session = cursor->session;

	once = false;
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
			return (util_cerr(cursor, "get_key", ret));
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(cursor, "get_value", ret));
		if (printf("%s%s%s%s%s%s", json && once ? "," : "",
		    prefix, key, infix, value, suffix) < 0)
			return (util_err(session, EIO, NULL));
		once = true;
	}
	if (json && once && printf("\n") < 0)
		return (util_err(session, EIO, NULL));
	return (ret == WT_NOTFOUND ? 0 :
	    util_cerr(cursor, (reverse ? "prev" : "next"), ret));
}

/*
 * dump_suffix --
 *	Output the dump file header suffix.
 */
static int
dump_suffix(WT_SESSION *session, bool json)
{
	if (json) {
		if (printf(
		    "        },\n"
		    "        {\n"
		    "            \"data\" : [") < 0)
			return (util_err(session, EIO, NULL));
	} else {
		if (printf("Data\n") < 0)
			return (util_err(session, EIO, NULL));
	}
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
		nchars += __wt_json_unpack_char((u_char)*p, NULL, 0, false);
	q = malloc(nchars + 1);
	if (q == NULL)
		return (1);
	*result = q;
	left = nchars;
	for (p = str; *p; p++, nchars++) {
		nchars = __wt_json_unpack_char((u_char)*p, (u_char *)q, left,
		    false);
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
print_config(WT_SESSION *session, const char *key, const char *cfg, bool json,
    bool toplevel)
{
	WT_DECL_RET;
	char *jsonconfig;

	/*
	 * We have all of the object configuration, but don't have the default
	 * session.create configuration. Have the underlying library add in the
	 * defaults and collapse it all into one load configuration string.
	 */
	jsonconfig = NULL;
	if (json && (ret = dup_json_string(cfg, &jsonconfig)) != 0)
		return (util_err(session, ret, NULL));

	if (json) {
		if (toplevel)
			ret = printf(
			    "    \"%s\" : [\n        {\n            "
			    "\"config\" : \"%s\",\n", key, jsonconfig);
		else
			ret = printf(
			    "                {\n"
			    "                    \"uri\" : \"%s\",\n"
			    "                    \"config\" : \"%s\"\n"
			    "                }", key, jsonconfig);
	} else
		ret = printf("%s\n%s\n", key, cfg);
	free(jsonconfig);
	if (ret < 0)
		return (util_err(session, EIO, NULL));
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
