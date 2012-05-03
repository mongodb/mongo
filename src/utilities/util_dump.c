/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int dump_config(WT_SESSION *, const char *);
static int dump_file_config(WT_SESSION *, const char *);
static int dump_prefix(int);
static int dump_suffix(void);
static int dump_table_config(WT_SESSION *, WT_CURSOR *, const char *);
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
	char *config, *name, *snapshot;

	hex = reverse = 0;
	config = name = snapshot = NULL;
	while ((ch = util_getopt(argc, argv, "f:rs:x")) != EOF)
		switch (ch) {
		case 'f':			/* output file */
			if (freopen(util_optarg, "w", stdout) == NULL)
				return (
				    util_err(errno, "%s: reopen", util_optarg));
			break;
		case 'r':
			reverse = 1;
			break;
		case 's':
			snapshot = util_optarg;
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
	if ((name =
	    util_name(*argv, "table", UTIL_FILE_OK | UTIL_TABLE_OK)) == NULL)
		goto err;

	if (dump_prefix(hex) != 0 ||
	    dump_config(session, name) != 0 ||
	    dump_suffix() != 0)
		goto err;

	len = snapshot == NULL ? 0 : strlen("snapshot=") + strlen(snapshot);
	len += strlen(hex ? "dump=hex" : "dump=print");
	if ((config = malloc(len + 10)) == NULL)
		goto err;
	if (snapshot == NULL)
		config[0] = '\0';
	else {
		(void)strcpy(config, "snapshot=");
		(void)strcat(config, snapshot);
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
 * config --
 *	Dump the config for the uri.
 */
static int
dump_config(WT_SESSION *session, const char *uri)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int tret;

	/* Dump the config. */
	if (strncmp(uri, "table:", strlen("table:")) == 0) {
		/* Open a metadata cursor. */
		if ((ret = session->open_cursor(
		    session, WT_METADATA_URI, NULL, NULL, &cursor)) != 0) {
			fprintf(stderr, "%s: %s: session.open_cursor: %s\n",
			    progname,
			    WT_METADATA_URI, wiredtiger_strerror(ret));
			return (1);
		}

		ret = dump_table_config(session, cursor, uri);

		if ((tret = cursor->close(cursor)) != 0 && ret == 0)
			ret = tret;
	} else
		ret = dump_file_config(session, uri);

	return (ret);
}

/*
 * dump_table_config --
 *	Dump the config for a table.
 */
static int
dump_table_config(WT_SESSION *session, WT_CURSOR *cursor, const char *uri)
{
	struct {
		char *key;			/* Metadata key */
		char *value;			/* Metadata value */
	} *list;
	WT_DECL_RET;
	int i, elem, list_elem;
	const char *key, *name, *value;
	char *buf, *filename, *p, *t, *sep;

	/* Get the name. */
	if ((name = strchr(uri, ':')) == NULL) {
		fprintf(stderr, "%s: %s: corrupted uri\n", progname, uri);
		return (1);
	}
	++name;

	list = NULL;
	elem = list_elem = 0;
	for (; (ret = cursor->next(cursor)) == 0; free(buf)) {
		/* Get the key and duplicate it, we want to overwrite it. */
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr(uri, "get_key", ret));
		if ((buf = strdup(key)) == NULL)
			return (util_err(errno, NULL));
			
		/* Check for the dump table's column groups or indices. */
		if ((p = strchr(buf, ':')) == NULL)
			continue;
		*p++ = '\0';
		if (strcmp(buf, "index") != 0 && strcmp(buf, "colgroup") != 0)
			continue;
		if ((t = strchr(p, ':')) == NULL)
			continue;
		*t++ = '\0';
		if (strcmp(p, name) != 0)
			continue;

		/* Found one, save it for review. */
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(uri, "get_value", ret));
		if (elem == list_elem && (list = realloc(list,
		    (size_t)(list_elem += 20) * sizeof(*list))) == NULL)
			return (util_err(errno, NULL));
		if ((list[elem].key = strdup(key)) == NULL)
			return (util_err(errno, NULL));
		if ((list[elem].value = strdup(value)) == NULL)
			return (util_err(errno, NULL));
		++elem;
	}
	if (ret != WT_NOTFOUND)
		return (util_cerr(uri, "next", ret));
	ret = 0;

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
	 * Second, dump the column group and index key/value pairs: for each
	 * one, look up the related file information and append it to the base
	 * record.
	 */
	for (i = 0; i < elem; ++i) {
		if ((filename = strstr(list[i].value, "filename=")) == NULL) {
			fprintf(stderr,
			    "%s: %s: has no underlying file configuration\n",
			    progname, list[i].key);
			return (1);
		}

		/*
		 * Nul-terminate the filename if necessary, create the file
		 * URI, then look it up.
		 */
		if ((sep = strchr(filename, ',')) != NULL)
			*sep = '\0';
		if ((t = strdup(filename)) == NULL)
			return (util_err(errno, NULL));
		if (sep != NULL)
			*sep = ',';
		p = t + strlen("filename=");
		p -= strlen("file:");
		memcpy(p, "file:", strlen("file:"));
		cursor->set_key(cursor, p);
		if ((ret = cursor->search(cursor)) != 0) {
			fprintf(stderr,
			    "%s: %s: unable to find metadata for the "
			    "underlying file %s\n",
			    progname, list[i].key, p);
			return (1);
		}
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr(uri, "get_value", ret));

		/*
		 * The dumped configuration string is the original key plus the
		 * file's configuration.
		 */
		if (print_config(
		    session, list[i].key, list[i].value, value) != 0)
			return (util_err(EIO, NULL));
	}

	/* Leak the memory, I don't care. */
	return (0);
}

/*
 * dump_file_config --
 *	Dump the config for a file.
 */
static int
dump_file_config(WT_SESSION *session, const char *uri)
{
	WT_DECL_RET;
	const char *value;

	/*
	 * We want to be able to dump the metadata file itself, but the
	 * configuration for that file lives in the turtle file.  Reach
	 * down into the library and ask for the file's configuration,
	 * that will work in all cases.
	 */
	if ((ret = __wt_file_metadata(session, uri, &value)) != 0)
		return (util_err(ret, "metadata read: %s", uri));

	/* Leak the memory, I don't care. */
	return (print_config(session, uri, value, NULL));
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
	    "dump [-rx] [-f output-file] [-s snapshot] uri\n",
	    progname, usage_prefix);
	return (1);
}
