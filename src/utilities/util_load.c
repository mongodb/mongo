/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int format(void);
static int insert(WT_CURSOR *, const char *);
static int load_dump(WT_SESSION *);
static int config_read(char ***, int *);
static int config_rename(char **, const char *);
static int config_update(WT_SESSION *, char **);
static int usage(void);

static int	append;		/* -a append (ignore record number keys) */
static char    *cmdname;	/* -r rename */
static char   **cmdconfig;	/* configuration pairs */
static int	no_overwrite;	/* -n don't overwrite existing data */

int
util_load(WT_SESSION *session, int argc, char *argv[])
{
	int ch;

	while ((ch = util_getopt(argc, argv, "af:nr:")) != EOF)
		switch (ch) {
		case 'a':	/* append (ignore record number keys) */
			append = 1;
			break;
		case 'f':	/* input file */
			if (freopen(util_optarg, "r", stdin) == NULL)
				return (
				    util_err(errno, "%s: reopen", util_optarg));
			break;
		case 'n':	/* don't overwrite existing data */
			no_overwrite = 1;
			break;
		case 'r':	/* rename */
			cmdname = util_optarg;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* -a and -o are mutually exclusive. */
	if (append == 1 && no_overwrite == 1)
		return (util_err(EINVAL,
		    "the -a (append) and -n (no-overwrite) flags are mutually "
		    "exclusive"));

	/* The remaining arguments are configuration uri/string pairs. */
	if (argc != 0) {
		if (argc % 2 != 0)
			return (usage());
		cmdconfig = argv;
	}

	return (load_dump(session));
}

/*
 * load_dump --
 *	Load from the WiredTiger dump format.
 */
static int
load_dump(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int hex, tret;
	char **entry, **list, *p, *uri, config[64];

	list = NULL;		/* -Wuninitialized */
	hex = 0;		/* -Wuninitialized */

	/* Read the metadata file. */
	if ((ret = config_read(&list, &hex)) != 0)
		return (ret);

	/*
	 * Search for a table name -- if we find one, then it's table dump,
	 * otherwise, it's a single file dump.
	 */
	for (entry = list; *entry != NULL; ++entry)
		if (WT_PREFIX_MATCH(*entry, "table:"))
			break;
	if (*entry == NULL) {
		/*
		 * Single file dumps can only have two lines, the file name and
		 * the configuration information.
		 */
		if ((list[0] == NULL || list[1] == NULL || list[2] != NULL) ||
		    (WT_PREFIX_MATCH(list[0], "file:") &&
		    WT_PREFIX_MATCH(list[0], "lsm:")))
			return (format());

		entry = list;
	}

	/*
	 * Make sure the table key/value pair comes first, then we can just
	 * run through the array in order.  (We already checked that we had
	 * a multiple of 2 entries, so this is safe.)
	 */
	if (entry != list) {
		p = list[0]; list[0] = entry[0]; entry[0] = p;
		p = list[1]; list[1] = entry[1]; entry[1] = p;
	}

	/* Update the config based on any command-line configuration. */
	if ((ret = config_update(session, list)) != 0)
		return (ret);

	uri = list[0];
	for (entry = list; *entry != NULL; entry += 2)
		if ((ret = session->create(session, entry[0], entry[1])) != 0)
			return (util_err(ret, "%s: session.create", entry[0]));

	/* Open the insert cursor. */
	(void)snprintf(config, sizeof(config),
	    "dump=%s%s%s",
	    hex ? "hex" : "print",
	    append ? ",append" : "", no_overwrite ? ",overwrite=false" : "");
	if ((ret = session->open_cursor(
	    session, uri, NULL, config, &cursor)) != 0)
		return (util_err(ret, "%s: session.open", uri));

	/*
	 * Check the append flag (it only applies to objects where the primary
	 * key is a record number).
	 */
	if (append && strcmp(cursor->key_format, "r") != 0) {
		fprintf(stderr,
		    "%s: %s: -a option illegal unless the primary key is a "
		    "record number\n",
		    progname, uri);
		ret = 1;
	} else
		ret = insert(cursor, uri);

	/*
	 * Technically, we don't have to close the cursor because the session
	 * handle will do it for us, but I'd like to see the flush to disk and
	 * the close succeed, it's better to fail early when loading files.
	 */
	if ((tret = cursor->close(cursor)) != 0) {
		tret = util_err(tret, "%s: cursor.close", uri);
		if (ret == 0)
			ret = tret;
	}
	if (ret == 0)
		ret = util_flush(session, uri);

	return (ret == 0 ? 0 : 1);
}

/*
 * config_read --
 *	Read the config lines and do some basic validation.
 */
static int
config_read(char ***listp, int *hexp)
{
	ULINE l;
	int entry, eof, max_entry;
	const char *s;
	char **list;

	memset(&l, 0, sizeof(l));

	/* Header line #1: "WiredTiger Dump" and a WiredTiger version. */
	if (util_read_line(&l, 0, &eof))
		return (1);
	s = "WiredTiger Dump ";
	if (strncmp(l.mem, s, strlen(s)) != 0)
		return (format());

	/* Header line #2: "Format={hex,print}". */
	if (util_read_line(&l, 0, &eof))
		return (1);
	if (strcmp(l.mem, "Format=print") == 0)
		*hexp = 0;
	else if (strcmp(l.mem, "Format=hex") == 0)
		*hexp = 1;
	else
		return (format());

	/* Header line #3: "Header". */
	if (util_read_line(&l, 0, &eof))
		return (1);
	if (strcmp(l.mem, "Header") != 0)
		return (format());

	/* Now, read in lines until we get to the end of the headers. */
	for (entry = max_entry = 0, list = NULL;; ++entry) {
		if (util_read_line(&l, 0, &eof))
			return (1);
		if (strcmp(l.mem, "Data") == 0)
			break;

		/* Grow the array of header lines as necessary. */
		if ((max_entry == 0 || entry == max_entry - 1) &&
		    (list = realloc(list,
		    (size_t)(max_entry += 100) * sizeof(char *))) == NULL)
			return (util_err(errno, NULL));
		if ((list[entry] = strdup(l.mem)) == NULL)
			return (util_err(errno, NULL));
	}
	list[entry] = NULL;
	*listp = list;

	/* The lines are supposed to be in pairs. */
	if (entry / 2 == 0)
		return (format());

	/* Leak the memory, I don't care. */
	return (0);
}

/*
 * config_update --
 *	Reconcile and update the command line configuration against the
 * config we found.
 */
static int
config_update(WT_SESSION *session, char **list)
{
	int found;
	const char *cfg[] = { NULL, NULL, NULL };
	char **configp, **listp, *p, *t;

	/*
	 * If the object has been renamed, replace all of the column group,
	 * index, file and table names with the new name.
	 */
	if (cmdname != NULL) {
		for (listp = list; *listp != NULL; listp += 2)
			if (WT_PREFIX_MATCH(*listp, "colgroup:") ||
			    WT_PREFIX_MATCH(*listp, "file:") ||
			    WT_PREFIX_MATCH(*listp, "index:") ||
			    WT_PREFIX_MATCH(*listp, "table:"))
				if (config_rename(listp, cmdname))
					return (1);

		/*
		 * If the object was renamed, and there are configuration pairs,
		 * rename the configuration pairs as well, because we don't know
		 * if the user used the old or new names for the pair's URI.
		 */
		for (configp = cmdconfig;
		    cmdconfig != NULL && *configp != NULL; configp += 2)
			if (config_rename(configp, cmdname))
				return (1);
	}

	/*
	 * Remove all "filename=" configurations from the values, new filenames
	 * are chosen as part of table load.
	 */
	for (listp = list; *listp != NULL; listp += 2)
		if ((p = strstr(listp[1], "filename=")) != NULL) {
			if ((t = strchr(p, ',')) == NULL)
				*p = '\0';
			else
				memmove(p, t + 1, strlen(t + 1) + 1);
		}

	/*
	 * It's possible to update everything except the key/value formats.
	 * If there were command-line configuration pairs, walk the list of
	 * command-line configuration strings, and check.
	 */
	for (configp = cmdconfig;
	    cmdconfig != NULL && *configp != NULL; configp += 2)
		if (strstr(configp[1], "key_format=") ||
		    strstr(configp[1], "value_format="))
			return (util_err(0,
			    "the command line configuration string may not "
			    "modify the object's key or value format"));

	/*
	 * If there were command-line configuration pairs, walk the list of
	 * command-line URIs and find a matching dump URI.  For each match,
	 * rewrite the dump configuration as described by the command-line
	 * configuration.  It is an error if a command-line URI doesn't find
	 * a single, exact match, that's likely a mistake.
	 */
	for (configp = cmdconfig;
	    cmdconfig != NULL && *configp != NULL; configp += 2) {
		found = 0;
		for (listp = list; *listp != NULL; listp += 2) {
			if (strncmp(*configp, listp[0], strlen(*configp)) != 0)
				continue;
			/*
			 * !!!
			 * We support JSON configuration strings, which leads to
			 * configuration strings with brackets.  Unfortunately,
			 * that implies we can't simply append new configuration
			 * strings to existing ones.  We call an unpublished
			 * WiredTiger API to do the concatenation: if anyone
			 * else ever needs it we can make it public, but I think
			 * that's unlikely.  We're also playing fast and loose
			 * with types, but it should work.
			 */
			cfg[0] = listp[1];
			cfg[1] = configp[1];
			if (__wt_config_concat(
			    (WT_SESSION_IMPL *)session, cfg,
			    (const char **)&listp[1]) != 0)
				return (1);
			++found;
		}
		switch (found) {
		case 0:
			return (util_err(0,
			    "the command line object name %s was not matched "
			    "by any loaded object name", *configp));
		case 1:
			break;
		default:
			return (util_err(0,
			    "the command line object name %s was not unique, "
			    "matching more than a single loaded object name",
			    *configp));
		}
	}

	/* Leak the memory, I don't care. */
	return (0);
}

/*
 * config_rename --
 *	Update the URI name.
 */
static int
config_rename(char **urip, const char *name)
{
	size_t len;
	char *buf, *p;

	/* Allocate room. */
	len = strlen(*urip) + strlen(name) + 10;
	if ((buf = malloc(len)) == NULL)
		return (util_err(errno, NULL));

	/*
	 * Find the separating colon characters, but not the trailing one may
	 * not be there.
	 */
	if ((p = strchr(*urip, ':')) == NULL)
		return (format());
	*p = '\0';
	p = strchr(p + 1, ':');
	snprintf(buf, len, "%s:%s%s", *urip, name, p == NULL ? "" : p);
	*urip = buf;

	return (0);
}

/*
 * format --
 *	The input doesn't match the dump format.
 */
static int
format(void)
{
	return (util_err(0, "input does not match WiredTiger dump format"));
}

/*
 * insert --
 *	Read and insert data.
 */
static int
insert(WT_CURSOR *cursor, const char *name)
{
	ULINE key, value;
	WT_DECL_RET;
	uint64_t insert_count;
	int eof;

	memset(&key, 0, sizeof(key));
	memset(&value, 0, sizeof(value));

	/* Read key/value pairs and insert them into the file. */
	for (insert_count = 0;;) {
		/*
		 * Three modes: in row-store, we always read a key and use it,
		 * in column-store, we might read it (a dump), we might read
		 * and ignore it (a dump with "append" set), or not read it at
		 * all (flat-text load).
		 */
		if (util_read_line(&key, 1, &eof))
			return (1);
		if (eof == 1)
			break;
		if (!append)
			cursor->set_key(cursor, key.mem);

		if (util_read_line(&value, 0, &eof))
			return (1);
		cursor->set_value(cursor, value.mem);

		if ((ret = cursor->insert(cursor)) != 0)
			return (util_err(ret, "%s: cursor.insert", name));

		/* Report on progress every 100 inserts. */
		if (verbose && ++insert_count % 100 == 0) {
			printf("\r\t%s: %" PRIu64, name, insert_count);
			fflush(stdout);
		}
	}

	if (verbose)
		printf("\r\t%s: %" PRIu64 "\n", name, insert_count);

	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "load [-as] [-f input-file] [-r name] [object configuration ...]\n",
	    progname, usage_prefix);
	return (1);
}
