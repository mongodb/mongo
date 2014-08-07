/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

/*
 * Encapsulates the input state for parsing JSON.
 *
 * At any time, we may be peeking at an unconsumed token; this is
 * indicated by 'peeking' as true.  toktype, tokstart, toklen will be
 * set in this case.
 *
 * Generally we are collecting and processing tokens one by one.
 * In JSON, tokens never span lines so this makes processing easy.
 * The exception is that a JSON dump cursor takes the complete
 * set of keys or values during cursor->set_key/set_value calls,
 * which may contain many tokens and span lines.  E.g.
 *   cursor->set_value("\"name\" : \"jo\", \"phone\" : 2348765");
 * The raw key/value string is collected in in the kvraw field.
 */
typedef struct {
	WT_SESSION *session;    /* associated session */
	ULINE line;		/* current line */
	const char *p;		/* points to cur position in line.mem */
	int ateof;		/* current token is EOF */
	int peeking;		/* peeking at next token */
	int toktype;		/* next token, defined by __wt_json_token() */
	const char *tokstart;	/* next token start (points into line.mem) */
	size_t toklen;		/* next token length */
	char *kvraw;		/* multiline raw content collected so far */
	size_t kvrawstart;	/* pos on cur line that JSON key/value starts */
	const char *filename;   /* filename for error reporting */
	int linenum;		/* line number for error reporting */
} JSON_INPUT_STATE;

/*
 * A list of configuration strings.
 */
typedef struct {
	char **list;		/* array of alternating (uri, config) values */
	int entry;		/* next entry available in list */
	int max_entry;		/* how many allocated in list */
} CONFIG_LIST;

static int config_list_add(CONFIG_LIST *, char *);
static int config_read(char ***, int *);
static int config_rename(char **, const char *);
static void config_remove(char *, char *);
static int config_reorder(char **list);
static int config_update(WT_SESSION *, char **);
static int format(void);
static int insert(WT_CURSOR *, const char *);
static int json_cgidx(WT_SESSION *, JSON_INPUT_STATE *, CONFIG_LIST *, int);
static int json_config(WT_SESSION *, JSON_INPUT_STATE *, char **);
static int json_data(WT_SESSION *, JSON_INPUT_STATE *, const char *);
static int json_expect(WT_SESSION *, JSON_INPUT_STATE *, int);
static int json_peek(WT_SESSION *, JSON_INPUT_STATE *);
static int json_kvraw_append(JSON_INPUT_STATE *, const char *, size_t);
static int json_strdup(JSON_INPUT_STATE *, char **);
static int json_top_level(WT_SESSION *, JSON_INPUT_STATE *);
static int load_dump(WT_SESSION *);
static int load_json(WT_SESSION *, const char *);
static int usage(void);

static int	append;		/* -a append (ignore record number keys) */
static char    *cmdname;	/* -r rename */
static char   **cmdconfig;	/* configuration pairs */
static int	json;		/* -j input is JSON format */
static int	no_overwrite;	/* -n don't overwrite existing data */

#define JSON_STRING_MATCH(ins, match)					\
	((ins)->toklen - 2 == strlen(match) &&				\
	    strncmp((ins)->tokstart + 1, (match), (ins)->toklen - 2) == 0)

#define JSON_INPUT_POS(ins)						\
	((int)((ins)->p - (const char *)(ins)->line.mem))

#define JSON_EXPECT(session, ins, tok) do {				\
	if (json_expect(session, ins, tok))				\
		goto err;						\
} while (0)

int
util_load(WT_SESSION *session, int argc, char *argv[])
{
	int ch;
	const char *filename;

	filename = "<stdin>";
	while ((ch = util_getopt(argc, argv, "af:jnr:")) != EOF)
		switch (ch) {
		case 'a':	/* append (ignore record number keys) */
			append = 1;
			break;
		case 'f':	/* input file */
			if (freopen(util_optarg, "r", stdin) == NULL)
				return (
				    util_err(errno, "%s: reopen", util_optarg));
			else
				filename = util_optarg;
			break;
		case 'j':	/* input file */
			json = 1;
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

	if (json)
		return (load_json(session, filename));
	else
		return (load_dump(session));
}

/*
 * json_cgidx --
 *	Parse a column group or index entry from JSON input.
 */
static int
json_cgidx(WT_SESSION *session, JSON_INPUT_STATE *ins, CONFIG_LIST *clp,
    int idx)
{
	WT_DECL_RET;
	char *config, *p, *uri;
	int isconfig;

	uri = NULL;
	config = NULL;

	while (json_peek(session, ins) == '{') {
		JSON_EXPECT(session, ins, '{');
		JSON_EXPECT(session, ins, 's');
		isconfig = JSON_STRING_MATCH(ins, "config");
		if (!isconfig && !JSON_STRING_MATCH(ins, "uri"))
			goto err;
		JSON_EXPECT(session, ins, ':');
		JSON_EXPECT(session, ins, 's');

		if ((ret = json_strdup(ins, &p)) != 0) {
			ret = util_err(ret, NULL);
			goto err;
		}
		if (isconfig)
			config = p;
		else
			uri = p;

		isconfig = !isconfig;
		JSON_EXPECT(session, ins, ',');
		JSON_EXPECT(session, ins, 's');
		if (!JSON_STRING_MATCH(ins, isconfig ? "config" : "uri"))
			goto err;
		JSON_EXPECT(session, ins, ':');
		JSON_EXPECT(session, ins, 's');

		if ((ret = json_strdup(ins, &p)) != 0) {
			ret = util_err(ret, NULL);
			goto err;
		}
		if (isconfig)
			config = p;
		else
			uri = p;
		JSON_EXPECT(session, ins, '}');
		if ((idx && strncmp(uri, "index:", 6) != 0) ||
		    (!idx && strncmp(uri, "colgroup:", 9) != 0)) {
			ret = util_err(EINVAL,
			    "%s: misplaced colgroup or index", uri);
			goto err;
		}
		if ((ret = config_list_add(clp, uri)) != 0 ||
		    (ret = config_list_add(clp, config)) != 0)
			goto err;

		if (json_peek(session, ins) != ',')
			break;
		JSON_EXPECT(session, ins, ',');
		if (json_peek(session, ins) != '{')
			goto err;
	}
	if (0) {
err:		if (ret == 0)
			ret = EINVAL;
	}
	return (ret);
}

/*
 * json_kvraw_append --
 *	Append to the kvraw buffer, which is used to collect all the
 *	raw key/value pairs from JSON input.
 */
static int json_kvraw_append(JSON_INPUT_STATE *ins, const char *str, size_t len)
{
	char *tmp;
	size_t needsize;

	if (len > 0) {
		needsize = strlen(ins->kvraw) + len + 2;
		if ((tmp = malloc(needsize)) == NULL)
			return (util_err(errno, NULL));
		snprintf(tmp, needsize, "%s %.*s", ins->kvraw, (int)len, str);
		free(ins->kvraw);
		ins->kvraw = tmp;
	}
	return (0);
}

/*
 * json_strdup --
 *	Return a string, with no escapes or other JSON-isms, from the
 *	JSON string at the current input position.
 */
static int
json_strdup(JSON_INPUT_STATE *ins, char **resultp)
{
	WT_DECL_RET;
	char *result, *resultcpy;
	const char *src;
	ssize_t resultlen, srclen;

	result = NULL;
	src = ins->tokstart + 1;  /*strip "" from token */
	srclen = ins->toklen - 2;
	if ((resultlen = __wt_json_strlen(src, srclen)) < 0) {
		ret = util_err(EINVAL, "Invalid config string");
		goto err;
	}
	resultlen += 1;
	if ((result = (char *)malloc(resultlen)) == NULL) {
		ret = util_err(errno, NULL);
		goto err;
	}
	*resultp = result;
	resultcpy = result;
	if ((ret = __wt_json_strncpy(&resultcpy, resultlen, src, srclen))
	    != 0) {
		ret = util_err(ret, NULL);
		goto err;
	}

	if (0) {
err:		if (ret == 0)
			ret = EINVAL;
		if (result != NULL)
			free(result);
		*resultp = NULL;
	}
	return (ret);
}

static int
config_list_add(CONFIG_LIST *clp, char *val)
{
	if (clp->entry + 1 >= clp->max_entry)
		if ((clp->list = realloc(clp->list, (size_t)
		    (clp->max_entry += 100) * sizeof(char *))) == NULL)
			/* List already freed by realloc. */
			return (util_err(errno, NULL));

	clp->list[clp->entry++] = val;
	clp->list[clp->entry] = NULL;
	return (0);
}

static void
config_list_free(CONFIG_LIST *clp)
{
	free(clp->list);
}

/*
 * json_config --
 *	Parse a set of configuration entries from JSON, and create any
 *	table, column groups or indices mentioned.  The table/file URI
 *	seen in the JSON file is an input/output parameter, as it may
 *	be renamed via the 'r' option.
 */
static int
json_config(WT_SESSION *session, JSON_INPUT_STATE *ins, char **urip)
{
	CONFIG_LIST cl;
	WT_DECL_RET;
	char *config, **entry, *uri;

	memset(&cl, 0, sizeof(cl));
	uri = NULL;
	while (json_peek(session, ins) == 's') {
		JSON_EXPECT(session, ins, 's');
		if (JSON_STRING_MATCH(ins, "config")) {
			JSON_EXPECT(session, ins, ':');
			JSON_EXPECT(session, ins, 's');
			if ((ret = json_strdup(ins, &config)) != 0) {
				ret = util_err(ret, NULL);
				goto err;
			}
			if ((uri = strdup(*urip)) == NULL) {
				ret = util_err(ret, NULL);
				goto err;
			}
			config_list_add(&cl, uri);
			config_list_add(&cl, config);
		} else if (JSON_STRING_MATCH(ins, "colgroups")) {
			JSON_EXPECT(session, ins, ':');
			JSON_EXPECT(session, ins, '[');
			if ((ret = json_cgidx(session, ins, &cl, 0)) != 0)
				goto err;
			JSON_EXPECT(session, ins, ']');
		} else if (JSON_STRING_MATCH(ins, "indices")) {
			JSON_EXPECT(session, ins, ':');
			JSON_EXPECT(session, ins, '[');
			if ((ret = json_cgidx(session, ins, &cl, 1)) != 0)
				goto err;
			JSON_EXPECT(session, ins, ']');
		} else
			goto err;
		if (json_peek(session, ins) != ',')
			break;
		JSON_EXPECT(session, ins, ',');
		if (json_peek(session, ins) != 's')
			goto err;
	}
	/* Reorder and check the list. */
	if ((ret = config_reorder(cl.list)) != 0)
		return (ret);

	/* Update the config based on any command-line configuration. */
	if ((ret = config_update(session, cl.list)) != 0)
		goto err;

	for (entry = cl.list; *entry != NULL; entry += 2)
		if ((ret = session->create(session, entry[0], entry[1])) != 0) {
			ret = util_err(ret, "%s: session.create", entry[0]);
			goto err;
		}

	*urip = cl.list[0];

	if (0) {
err:		if (ret == 0)
			ret = EINVAL;
	}
	if (cl.list != NULL)
		/* keep cl.list[0], that's returned as the new URI */
		for (entry = &cl.list[1]; *entry != NULL; entry++)
			free(*entry);

	config_list_free(&cl);
	return (ret);
}

/*
 * json_data --
 *	Parse the data portion of the JSON input, and insert all
 *	values.
 */
static int
json_data(WT_SESSION *session, JSON_INPUT_STATE *ins, const char *uri)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	char config[64], *endp;
	const char *keyformat;
	int isrec, nfield, nkeys, toktype, tret;
	size_t gotnolen, keystrlen;
	uint64_t gotno, recno;

	cursor = NULL;
	(void)snprintf(config, sizeof(config),
	    "dump=json%s%s",
	    append ? ",append" : "", no_overwrite ? ",overwrite=false" : "");
	if ((ret = session->open_cursor(
	    session, uri, NULL, config, &cursor)) != 0) {
		ret = util_err(ret, "%s: session.open", uri);
		goto err;
	}
	keyformat = cursor->key_format;
	isrec = (strcmp(keyformat, "r") == 0);
	for (nkeys = 0; *keyformat; keyformat++)
		if (!isdigit(*keyformat))
			nkeys++;

	recno = 0;
	while (json_peek(session, ins) == '{') {
		nfield = 0;
		JSON_EXPECT(session, ins, '{');
		if ((ins)->kvraw == NULL)
			(ins)->kvraw = (char *)malloc(1);
		(ins)->kvraw[0] = '\0';
		(ins)->kvrawstart = JSON_INPUT_POS(ins);
		keystrlen = 0;
		while (json_peek(session, ins) == 's') {
			JSON_EXPECT(session, ins, 's');
			JSON_EXPECT(session, ins, ':');
			toktype = json_peek(session, ins);
			JSON_EXPECT(session, ins, toktype);
			if (isrec && nfield == 0) {
				/* Verify the dump has recnos in order. */
				recno++;
				gotno = __wt_strtouq(ins->tokstart, &endp, 0);
				gotnolen = (endp - ins->tokstart);
				if (recno != gotno || ins->toklen != gotnolen) {
					ret = util_err(0,
					    "%s: recno out of order", uri);
					goto err;
				}
			}
			if (++nfield == nkeys) {
				int curpos = JSON_INPUT_POS(ins);
				if ((ret = json_kvraw_append(ins,
				    (char *)(ins)->line.mem + (ins)->kvrawstart,
				    curpos - (ins)->kvrawstart)) != 0)
					goto err;
				ins->kvrawstart = curpos;
				keystrlen = strlen(ins->kvraw);
			}
			if (json_peek(session, ins) != ',')
				break;
			JSON_EXPECT(session, ins, ',');
			if (json_peek(session, ins) != 's')
				goto err;
		}
		if (json_kvraw_append(ins, ins->line.mem, JSON_INPUT_POS(ins)))
			goto err;

		ins->kvraw[keystrlen] = '\0';
		if (!append)
			cursor->set_key(cursor, ins->kvraw);
		/* skip over inserted space and comma */
		cursor->set_value(cursor, &ins->kvraw[keystrlen+2]);
		if ((ret = cursor->insert(cursor)) != 0) {
			ret = util_err(ret, "%s: cursor.insert", uri);
			goto err;
		}

		JSON_EXPECT(session, ins, '}');
		if (json_peek(session, ins) != ',')
			break;
		JSON_EXPECT(session, ins, ',');
		if (json_peek(session, ins) != '{')
			goto err;
	}
	if (0) {
err:		if (ret == 0)
			ret = EINVAL;
	}
	/*
	 * Technically, we don't have to close the cursor because the session
	 * handle will do it for us, but I'd like to see the flush to disk and
	 * the close succeed, it's better to fail early when loading files.
	 */
	if (cursor != NULL && (tret = cursor->close(cursor)) != 0) {
		tret = util_err(tret, "%s: cursor.close", uri);
		if (ret == 0)
			ret = tret;
	}
	if (ret == 0)
		ret = util_flush(session, uri);
	return (ret);
}

/*
 * json_top_level --
 *	Parse the top level JSON input.
 */
static int
json_top_level(WT_SESSION *session, JSON_INPUT_STATE *ins)
{
	WT_DECL_RET;
	char *tableuri;

	tableuri = NULL;
	JSON_EXPECT(session, ins, '{');
	while (json_peek(session, ins) == 's') {
		JSON_EXPECT(session, ins, 's');
		tableuri = realloc(tableuri, ins->toklen);
		snprintf(tableuri, ins->toklen, "%.*s",
		    (int)(ins->toklen - 2), ins->tokstart + 1);
		JSON_EXPECT(session, ins, ':');
		JSON_EXPECT(session, ins, '[');
		JSON_EXPECT(session, ins, '{');
		if ((ret = json_config(session, ins, &tableuri)) != 0)
			goto err;
		JSON_EXPECT(session, ins, '}');
		JSON_EXPECT(session, ins, ',');
		JSON_EXPECT(session, ins, '[');
		if ((ret = json_data(session, ins, tableuri)) != 0)
			goto err;
		JSON_EXPECT(session, ins, ']');
		JSON_EXPECT(session, ins, ']');
		if (json_peek(session, ins) != ',')
			break;
		JSON_EXPECT(session, ins, ',');
		if (json_peek(session, ins) != 's')
			goto err;
	}
	JSON_EXPECT(session, ins, '}');
	JSON_EXPECT(session, ins, 0);

	if (0) {
err:		if (ret == 0)
			ret = EINVAL;
	}
	free(tableuri);
	return (ret);
}

/*
 * json_peek --
 *	Set the input state to the next available token in the input
 *	and return its tokentype, a code defined by __wt_json_token().
 */
static int
json_peek(WT_SESSION *session, JSON_INPUT_STATE *ins)
{
	int ret = 0;

	if (!ins->peeking) {
		while (!ins->ateof) {
			while (isspace(*ins->p))
				ins->p++;
			if (*ins->p)
				break;
			if (ins->kvraw != NULL) {
				if (json_kvraw_append(ins,
				    (char *)ins->line.mem + ins->kvrawstart,
				    strlen(ins->line.mem) - ins->kvrawstart)) {
					ret = -1;
					goto err;
				}
				ins->kvrawstart = 0;
			}
			if (util_read_line(&ins->line, 1,
			    &ins->ateof)) {
				ins->toktype = -1;
				ret = -1;
				goto err;
			}
			ins->linenum++;
			ins->p = (const char *)ins->line.mem;
		}
		if (ins->ateof)
			ins->toktype = 0;
		else if (__wt_json_token(session, ins->p,
		    &ins->toktype, &ins->tokstart,
		    &ins->toklen) != 0)
			ins->toktype = -1;
		ins->peeking = 1;
	}
	if (0) {
	err:	if (ret == 0)
			ret = -1;
	}
	return (ret == 0 ? ins->toktype : -1);
}

/*
 * json_expect --
 *	Ensure that the type of the next token in the input matches
 *	the wanted value, and advance past it.  The values of the
 *	input state will be set so specific string or integer values
 *	can be pulled out after this call.
 */
static int
json_expect(WT_SESSION *session, JSON_INPUT_STATE *ins, int wanttok)
{
	if (json_peek(session, ins) < 0)
		return (1);
	ins->p += ins->toklen;
	ins->peeking = 0;
	if (ins->toktype != wanttok) {
		fprintf(stderr,
		    "%s: %d: %d: expected %s, got %s\n",
		    ins->filename,
		    ins->linenum,
		    JSON_INPUT_POS(ins) + 1,
		    __wt_json_tokname(wanttok),
		    __wt_json_tokname(ins->toktype));
		return (1);
	}
	return (0);
}


/*
 * load_json --
 *	Load from the JSON format produced by 'wt dump -j'.
 */
static int
load_json(WT_SESSION *session, const char *filename)
{
	JSON_INPUT_STATE instate;
	WT_DECL_RET;

	memset(&instate, 0, sizeof(instate));
	instate.session = session;
	if (util_read_line(&instate.line, 0, &instate.ateof))
		return (1);
	instate.p = (const char *)instate.line.mem;
	instate.linenum = 1;
	instate.filename = filename;

	if ((ret = json_top_level(session, &instate)) != 0)
		goto err;

err:	if (instate.line.mem != NULL)
		free(instate.line.mem);
	free(instate.kvraw);
	return (ret);
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
	char **entry, **list, **tlist, *uri, config[64];

	cursor = NULL;
	list = NULL;		/* -Wuninitialized */
	hex = 0;		/* -Wuninitialized */
	uri = NULL;

	/* Read the metadata file. */
	if ((ret = config_read(&list, &hex)) != 0)
		return (ret);

	/* Reorder and check the list. */
	if ((ret = config_reorder(list)) != 0)
		return (ret);

	/* Update the config based on any command-line configuration. */
	if ((ret = config_update(session, list)) != 0)
		goto err;

	uri = list[0];
	for (entry = list; *entry != NULL; entry += 2)
		if ((ret = session->create(session, entry[0], entry[1])) != 0) {
			ret = util_err(ret, "%s: session.create", entry[0]);
			goto err;
		}

	/* Open the insert cursor. */
	(void)snprintf(config, sizeof(config),
	    "dump=%s%s%s",
	    hex ? "hex" : "print",
	    append ? ",append" : "", no_overwrite ? ",overwrite=false" : "");
	if ((ret = session->open_cursor(
	    session, uri, NULL, config, &cursor)) != 0) {
		ret = util_err(ret, "%s: session.open", uri);
		goto err;
	}

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

err:	/*
	 * Technically, we don't have to close the cursor because the session
	 * handle will do it for us, but I'd like to see the flush to disk and
	 * the close succeed, it's better to fail early when loading files.
	 */
	if (cursor != NULL && (tret = cursor->close(cursor)) != 0) {
		tret = util_err(tret, "%s: cursor.close", uri);
		if (ret == 0)
			ret = tret;
	}
	if (ret == 0)
		ret = util_flush(session, uri);

	for (tlist = list; *tlist != NULL; ++tlist)
		free(*tlist);
	free(list);

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
	WT_DECL_RET;
	int entry, eof, max_entry;
	const char *s;
	char **list, **tlist;

	list = NULL;
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
		if ((ret = util_read_line(&l, 0, &eof)) != 0)
			goto err;
		if (strcmp(l.mem, "Data") == 0)
			break;

		/*
		 * Grow the array of header lines as necessary -- we need an
		 * extra slot for NULL termination.
		 */
		if (entry + 1 >= max_entry) {
			if ((tlist = realloc(list, (size_t)
			    (max_entry += 100) * sizeof(char *))) == NULL) {
				ret = util_err(errno, NULL);

				/*
				 * List already freed by realloc, still use err
				 * label for consistency.
				 */
				list = NULL;
				goto err;
			}
			list = tlist;
		}
		if ((list[entry] = strdup(l.mem)) == NULL) {
			ret = util_err(errno, NULL);
			goto err;
		}
		list[entry + 1] = NULL;
	}

	/* Headers are required, and they're supposed to be in pairs. */
	if (list == NULL || entry % 2 != 0) {
		ret = format();
		goto err;
	}
	*listp = list;
	return (0);

err:	if (list != NULL) {
		for (tlist = list; *tlist != NULL; ++tlist)
			free(*tlist);
		free(list);
	}
	return (ret);
}

/*
 * config_reorder --
 *	For table dumps, reorder the list so tables are first.
 *	For other dumps, make any needed checks.
 */
static int
config_reorder(char **list)
{
	char **entry, *p;

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
	return (0);
}

/*
 * config_update --
 *	Reconcile and update the command line configuration against the
 *	config we found.
 */
static int
config_update(WT_SESSION *session, char **list)
{
	int found;
	const char *cfg[] = { NULL, NULL, NULL };
	char **configp, **listp, **rm;
	static char *rmnames[] = {
		"filename", "id", "checkpoint",	"checkpoint_lsn",
		"version", "source", NULL };

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
	 * Remove all "filename=", "source=" and other configurations
	 * that foil loading from the values. New filenames are chosen
	 * as part of table load.
	 */
	for (listp = list; *listp != NULL; listp += 2)
		for (rm = rmnames; *rm != NULL; rm++)
			if (strstr(listp[1], *rm) != NULL)
				config_remove(listp[1], *rm);

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
	if ((p = strchr(*urip, ':')) == NULL) {
		free(buf);
		return (format());
	}
	*p = '\0';
	p = strchr(p + 1, ':');
	snprintf(buf, len, "%s:%s%s", *urip, name, p == NULL ? "" : p);
	*urip = buf;

	return (0);
}

/*
 * config_remove --
 *	Remove a single config key and its value.
 */
static void
config_remove(char *config, char *ckey)
{
	int parens, quoted;
	char *begin, match[100], *next, *p;

	snprintf(match, sizeof(match), "%s=", ckey);
	if ((begin = strstr(config, match)) != NULL) {
		parens = 0;
		quoted = 0;
		next = NULL;
		for (p = begin + strlen(match); !next && *p; p++)
			switch (*p) {
			case '(':
				if (!quoted)
					parens++;
				break;
			case ')':
				if (!quoted)
					parens--;
				break;
			case '"':
				quoted = !quoted;
				break;
			case ',':
				if (!quoted && parens == 0)
					next = p + 1;
				break;
			}
		if (next)
			memmove(begin, next, strlen(next) + 1);
		else
			*begin = '\0';
	}
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
