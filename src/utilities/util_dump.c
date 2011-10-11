/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

static int usage(void);

static inline int
dump_forward(WT_CURSOR *cursor, int dump_key)
{
	const char *key, *value;
	int ret;

	while ((ret = cursor->next(cursor)) == 0) {
		if (dump_key) {
			if ((ret = cursor->get_key(cursor, &key)) != 0)
				return (ret);
			puts(key);
		}
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (ret);
		puts(value);
	}
	return (ret);
}

static inline int
dump_reverse(WT_CURSOR *cursor, int dump_key)
{
	const char *key, *value;
	int ret;

	while ((ret = cursor->prev(cursor)) == 0) {
		if (dump_key) {
			if ((ret = cursor->get_key(cursor, &key)) != 0)
				return (ret);
			puts(key);
		}
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (ret);
		puts(value);
	}
	return (ret);
}

/*
 * schema --
 *	Dump the schema for the table.
 */
static int
schema(WT_SESSION *session, const char *name)
{
	struct {
		char *key;			/* Schema key */
		char *value;			/* Schema value */
	} *list;
	WT_CURSOR *cursor;
	const char *key, *value;
	int i, elem, list_elem, ret, tret;
	char *buf, *p, *t;

	ret = 0;

	/* Open the schema file. */
	if ((ret = session->open_cursor(
	    session, "file:__schema.wt", NULL, NULL, &cursor)) != 0)
		return (ret);

	/*
	 * XXX
	 * We're walking the entire schema file: I'd rather call the search_near
	 * function, but it doesn't guarantee less-than-or-equal-to semantics so
	 * the loop is hard to write.
	 */
	list = NULL;
	elem = list_elem = 0;
	for (; (ret = cursor->next(cursor)) == 0; free(buf)) {
		/* Get the key and duplicate it, we want to overwrite it. */
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			goto err;
		if ((buf = strdup(key)) == NULL)
			goto err;
			
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
			goto err;
		if (elem == list_elem && (list = realloc(list,
		    (size_t)(list_elem += 20) * sizeof(*list))) == NULL)
			goto err;
		if ((list[elem].key = strdup(key)) == NULL)
			goto err;
		if ((list[elem].value = strdup(value)) == NULL)
			goto err;
		++elem;
	}
	if (ret != WT_NOTFOUND)
		goto err;
	ret = 0;

	/*
	 * Dump out the schema information.
	 *
	 * 1) Dump the table's information (requires a lookup)
	 * 2) Dump the column group and index key/value pairs
	 * 3) Dump the underlying file information (requires a lookup)
	 */
	if ((buf = malloc(strlen("table:") + strlen(name) + 10)) == NULL)
		goto err;
	strcpy(buf, "table:");
	strcpy(buf + strlen("table:"), name);
	cursor->set_key(cursor, buf);
	if ((ret = cursor->search(cursor)) != 0) {
		fprintf(stderr,
		    "Unable to find schema reference for table %s\n", name);
		goto err;
	}
	if ((ret = cursor->get_key(cursor, &key)) != 0)
		goto err;
	if ((ret = cursor->get_value(cursor, &value)) != 0)
		goto err;
	printf("%s\n%s\n", key, value);

	for (i = 0; i < elem; ++i) {
		printf("%s\n%s\n", list[i].key, list[i].value);

		if ((p = strstr(list[i].value, "filename=")) != NULL) {
			if ((t = strchr(p, ',')) != NULL)
				*t = '\0';
			p += strlen("filename=");
			p -= strlen("file:");
			memcpy(p, "file:", strlen("file:"));
			cursor->set_key(cursor, p);
			if ((ret = cursor->search(cursor)) != 0) {
				fprintf(stderr,
				    "Unable to find schema reference for "
				    "underlying file %s\n", p);
				goto err;
			}
			if ((ret = cursor->get_key(cursor, &key)) != 0)
				goto err;
			if ((ret = cursor->get_value(cursor, &value)) != 0)
				goto err;
			printf("%s\n%s\n", key, value);
		}
	}

err:	if (cursor != NULL &&
	    (tret = cursor->close(cursor, NULL)) != 0 && ret == 0)
		ret = tret;

	/* Leak the memory, I don't care. */
	return (ret);
}

int
util_dump(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	int ch, dump_key, ret, reverse;
	const char *fmt;
	char *name;

	dump_key = reverse = 0;
	name = NULL;
	fmt = "dump=print";
	while ((ch = util_getopt(argc, argv, "f:kxr")) != EOF)
		switch (ch) {
		case 'f':			/* output file */
			if (freopen(util_optarg, "w", stdout) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, util_optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'k':
			dump_key = 1;
			break;
		case 'x':
			fmt = "dump=hex";
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

	/* The remaining argument is the uri. */
	if (argc != 1)
		return (usage());
	if ((name = util_name(
	    *argv, "table", UTIL_FILE_OK | UTIL_TABLE_OK)) == NULL)
		goto err;

	if (strncmp(name, "table:", strlen("table:")) == 0) {
		dump_key = 1;

		printf("WiredTiger Dump %s\n",
		    wiredtiger_version(NULL, NULL, NULL));
		printf("Header\n");
		if ((ret = schema(session, *argv)) != 0)
			goto err;
		printf("Data\n");
	}

	if ((ret =
	    session->open_cursor(session, name, NULL, fmt, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}

	if (strcmp(cursor->key_format, "r") != 0)
		dump_key = 1;

	if (reverse)
		ret = dump_reverse(cursor, dump_key);
	else
		ret = dump_forward(cursor, dump_key);

	if (ret == WT_NOTFOUND)
		ret = 0;
	else {
		fprintf(stderr, "%s: cursor get(%s) failed: %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}

	if (0) {
err:		ret = 1;
	}

	if (name != NULL)
		free(name);

	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "dump [-krx] [-f output-file] table\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
