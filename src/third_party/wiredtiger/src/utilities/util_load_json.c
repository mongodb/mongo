/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"
#include "util_dump.h"
#include "util_load.h"

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
 *   cursor->set_value("\"name\" : \"John\", \"phone\" : 2348765");
 * The raw key/value string is collected in the kvraw field.
 */
typedef struct {
    WT_SESSION *session;  /* associated session */
    ULINE line;           /* current line */
    const char *p;        /* points to cur position in line.mem */
    bool ateof;           /* current token is EOF */
    bool peeking;         /* peeking at next token */
    int toktype;          /* next token, defined by __wt_json_token() */
    const char *tokstart; /* next token start (points into line.mem) */
    size_t toklen;        /* next token length */
    char *kvraw;          /* multiple line raw content collected so far */
    size_t kvrawstart;    /* pos on cur line that JSON key/value starts */
    const char *filename; /* filename for error reporting */
    int linenum;          /* line number for error reporting */
} JSON_INPUT_STATE;

static int json_column_group_index(WT_SESSION *, JSON_INPUT_STATE *, CONFIG_LIST *, int);
static int json_data(WT_SESSION *, JSON_INPUT_STATE *, CONFIG_LIST *, uint32_t);
static int json_expect(WT_SESSION *, JSON_INPUT_STATE *, int);
static int json_peek(WT_SESSION *, JSON_INPUT_STATE *);
static int json_skip(WT_SESSION *, JSON_INPUT_STATE *, const char **);
static int json_kvraw_append(WT_SESSION *, JSON_INPUT_STATE *, const char *, size_t);
static int json_strdup(WT_SESSION *, JSON_INPUT_STATE *, char **);
static int json_top_level(WT_SESSION *, JSON_INPUT_STATE *, uint32_t);

#define JSON_STRING_MATCH(ins, match)      \
    ((ins)->toklen - 2 == strlen(match) && \
      strncmp((ins)->tokstart + 1, (match), (ins)->toklen - 2) == 0)

#define JSON_INPUT_POS(ins) ((size_t)((ins)->p - (const char *)(ins)->line.mem))

#define JSON_EXPECT(session, ins, tok)      \
    do {                                    \
        if (json_expect(session, ins, tok)) \
            goto err;                       \
    } while (0)

/*
 * json_column_group_index --
 *     Parse a column group or index entry from JSON input.
 */
static int
json_column_group_index(WT_SESSION *session, JSON_INPUT_STATE *ins, CONFIG_LIST *clp, int idx)
{
    WT_DECL_RET;
    char *config, *p, *uri;
    bool isconfig;

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

        if ((ret = json_strdup(session, ins, &p)) != 0) {
            ret = util_err(session, ret, NULL);
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

        if ((ret = json_strdup(session, ins, &p)) != 0) {
            ret = util_err(session, ret, NULL);
            goto err;
        }
        if (isconfig)
            config = p;
        else
            uri = p;
        JSON_EXPECT(session, ins, '}');
        if ((idx && strncmp(uri, "index:", 6) != 0) ||
          (!idx && strncmp(uri, "colgroup:", 9) != 0)) {
            ret = util_err(session, EINVAL, "%s: misplaced colgroup or index", uri);
            goto err;
        }
        if ((ret = config_list_add(session, clp, uri)) != 0 ||
          (ret = config_list_add(session, clp, config)) != 0)
            goto err;

        if (json_peek(session, ins) != ',')
            break;
        JSON_EXPECT(session, ins, ',');
        if (json_peek(session, ins) != '{')
            goto err;
    }
    if (0) {
err:
        if (ret == 0)
            ret = EINVAL;
    }
    return (ret);
}

/*
 * json_kvraw_append --
 *     Append to the kvraw buffer, which is used to collect all the raw key/value pairs from JSON
 *     input.
 */
static int
json_kvraw_append(WT_SESSION *session, JSON_INPUT_STATE *ins, const char *str, size_t len)
{
    WT_DECL_RET;
    size_t needsize;
    char *tmp;

    if (len > 0) {
        needsize = strlen(ins->kvraw) + len + 2;
        if ((tmp = malloc(needsize)) == NULL)
            return (util_err(session, errno, NULL));
        WT_ERR(__wt_snprintf(tmp, needsize, "%s %.*s", ins->kvraw, (int)len, str));
        free(ins->kvraw);
        ins->kvraw = tmp;
    }
    return (0);

err:
    free(tmp);
    return (util_err(session, ret, NULL));
}

/*
 * json_strdup --
 *     Return a string, with no escapes or other JSON-isms, from the JSON string at the current
 *     input position.
 */
static int
json_strdup(WT_SESSION *session, JSON_INPUT_STATE *ins, char **resultp)
{
    WT_DECL_RET;
    size_t srclen;
    ssize_t resultlen;
    char *result, *resultcpy;
    const char *src;

    result = NULL;
    src = ins->tokstart + 1; /*strip "" from token */
    srclen = ins->toklen - 2;
    if ((resultlen = __wt_json_strlen(src, srclen)) < 0) {
        ret = util_err(session, EINVAL, "Invalid config string");
        goto err;
    }
    resultlen += 1;
    if ((result = malloc((size_t)resultlen)) == NULL) {
        ret = util_err(session, errno, NULL);
        goto err;
    }
    *resultp = result;
    resultcpy = result;
    if ((ret = __wt_json_strncpy(session, &resultcpy, (size_t)resultlen, src, srclen)) != 0) {
        ret = util_err(session, ret, NULL);
        goto err;
    }

    if (0) {
err:
        if (ret == 0)
            ret = EINVAL;
        free(result);
        *resultp = NULL;
    }
    return (ret);
}

/*
 * json_data --
 *     Parse the data portion of the JSON input, and insert all values.
 */
static int
json_data(WT_SESSION *session, JSON_INPUT_STATE *ins, CONFIG_LIST *clp, uint32_t flags)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    size_t gotnolen, keystrlen;
    uint64_t gotno, recno;
    int nfield, nkeys, toktype, tret;
    char config[64], *endp, *uri;
    const char *keyformat;
    bool isrec;

    cursor = NULL;
    uri = NULL;

    /* Reorder and check the list. */
    if ((ret = config_reorder(session, clp->list)) != 0)
        goto err;

    /* Update config based on command-line configuration. */
    if ((ret = config_update(session, clp->list)) != 0)
        goto err;

    /* Create the items collected. */
    if ((ret = config_exec(session, clp->list)) != 0)
        goto err;

    uri = clp->list[0];
    if ((ret = __wt_snprintf(config, sizeof(config), "dump=json%s%s",
           LF_ISSET(LOAD_JSON_APPEND) ? ",append" : "",
           LF_ISSET(LOAD_JSON_NO_OVERWRITE) ? ",overwrite=false" : "")) != 0) {
        ret = util_err(session, ret, NULL);
        goto err;
    }
    if ((ret = session->open_cursor(session, uri, NULL, config, &cursor)) != 0) {
        ret = util_err(session, ret, "%s: session.open_cursor", uri);
        goto err;
    }
    keyformat = cursor->key_format;
    isrec = WT_STREQ(keyformat, "r");
    for (nkeys = 0; *keyformat; keyformat++)
        if (!__wt_isdigit((u_char)*keyformat))
            nkeys++;

    recno = 0;
    while (json_peek(session, ins) == '{') {
        nfield = 0;
        JSON_EXPECT(session, ins, '{');
        if (ins->kvraw == NULL) {
            if ((ins->kvraw = malloc(1)) == NULL) {
                ret = util_err(session, errno, NULL);
                goto err;
            }
        }
        ins->kvraw[0] = '\0';
        ins->kvrawstart = JSON_INPUT_POS(ins);
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
                gotnolen = (size_t)(endp - ins->tokstart);
                if (recno != gotno || ins->toklen != gotnolen) {
                    ret = util_err(session, 0, "%s: recno out of order", uri);
                    goto err;
                }
            }
            if (++nfield == nkeys) {
                size_t curpos = JSON_INPUT_POS(ins);
                if ((ret = json_kvraw_append(session, ins, (char *)ins->line.mem + ins->kvrawstart,
                       curpos - ins->kvrawstart)) != 0)
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
        if (json_kvraw_append(session, ins, ins->line.mem, JSON_INPUT_POS(ins)))
            goto err;

        ins->kvraw[keystrlen] = '\0';
        if (!LF_ISSET(LOAD_JSON_APPEND))
            cursor->set_key(cursor, ins->kvraw);
        /* skip over inserted space and comma */
        cursor->set_value(cursor, &ins->kvraw[keystrlen + 2]);
        if ((ret = cursor->insert(cursor)) != 0) {
            ret = util_err(session, ret, "%s: cursor.insert", uri);
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
err:
        if (ret == 0)
            ret = EINVAL;
    }
    /*
     * Technically, we don't have to close the cursor because the session handle will do it for us,
     * but I'd like to see the flush to disk and the close succeed, it's better to fail early when
     * loading files.
     */
    if (cursor != NULL && (tret = cursor->close(cursor)) != 0) {
        tret = util_err(session, tret, "%s: cursor.close", uri);
        if (ret == 0)
            ret = tret;
    }
    if (ret == 0)
        ret = util_flush(session, uri);
    return (ret);
}

/*
 * json_top_level --
 *     Parse the top level JSON input.
 */
static int
json_top_level(WT_SESSION *session, JSON_INPUT_STATE *ins, uint32_t flags)
{
    CONFIG_LIST cl;
    WT_DECL_RET;
    static const char *json_markers[] = {
      "\"config\"", "\"colgroups\"", "\"indices\"", "\"data\"", NULL};
    uint64_t curversion;
    int toktype;
    char *config, *tableuri;
    bool hasversion;

    memset(&cl, 0, sizeof(cl));
    tableuri = NULL;
    hasversion = false;

    JSON_EXPECT(session, ins, '{');
    while (json_peek(session, ins) == 's') {
        JSON_EXPECT(session, ins, 's');
        tableuri = realloc(tableuri, ins->toklen);
        if ((ret = __wt_snprintf(
               tableuri, ins->toklen, "%.*s", (int)(ins->toklen - 2), ins->tokstart + 1)) != 0) {
            ret = util_err(session, ret, NULL);
            goto err;
        }
        JSON_EXPECT(session, ins, ':');
        if (!hasversion) {
            if (strcmp(tableuri, DUMP_JSON_VERSION_MARKER) != 0) {
                ret = util_err(session, ENOTSUP, "missing \"%s\"", DUMP_JSON_VERSION_MARKER);
                goto err;
            }
            hasversion = true;
            JSON_EXPECT(session, ins, 's');
            if ((ret = util_str2num(session, ins->tokstart + 1, false, &curversion)) != 0)
                goto err;
            if (curversion > DUMP_JSON_SUPPORTED_VERSION) {
                ret = util_err(session, ENOTSUP, "unsupported JSON dump version \"%.*s\"",
                  (int)(ins->toklen - 1), ins->tokstart + 1);
                goto err;
            }
            JSON_EXPECT(session, ins, ',');
            continue;
        }

        /*
         * Allow any ordering of 'config', 'colgroups', 'indices' before 'data', which must appear
         * last. The non-'data' items build up a list of entries that created in our session before
         * the data is inserted.
         */
        for (;;) {
            if (json_skip(session, ins, json_markers) != 0)
                goto err;
            JSON_EXPECT(session, ins, 's');
            if (JSON_STRING_MATCH(ins, "config")) {
                JSON_EXPECT(session, ins, ':');
                JSON_EXPECT(session, ins, 's');
                if ((ret = json_strdup(session, ins, &config)) != 0) {
                    ret = util_err(session, ret, NULL);
                    goto err;
                }
                if ((ret = config_list_add(session, &cl, tableuri)) != 0)
                    goto err;
                if ((ret = config_list_add(session, &cl, config)) != 0)
                    goto err;
                tableuri = NULL;
            } else if (JSON_STRING_MATCH(ins, "colgroups")) {
                JSON_EXPECT(session, ins, ':');
                JSON_EXPECT(session, ins, '[');
                if ((ret = json_column_group_index(session, ins, &cl, 0)) != 0)
                    goto err;
                JSON_EXPECT(session, ins, ']');
            } else if (JSON_STRING_MATCH(ins, "indices")) {
                JSON_EXPECT(session, ins, ':');
                JSON_EXPECT(session, ins, '[');
                if ((ret = json_column_group_index(session, ins, &cl, 1)) != 0)
                    goto err;
                JSON_EXPECT(session, ins, ']');
            } else if (JSON_STRING_MATCH(ins, "data")) {
                JSON_EXPECT(session, ins, ':');
                JSON_EXPECT(session, ins, '[');
                if ((ret = json_data(session, ins, &cl, flags)) != 0)
                    goto err;
                config_list_free(&cl);
                free(ins->kvraw);
                ins->kvraw = NULL;
                config_list_free(&cl);
                break;
            } else
                goto err;
        }

        while ((toktype = json_peek(session, ins)) == '}' || toktype == ']')
            JSON_EXPECT(session, ins, toktype);
        if (toktype == 0) /* Check EOF. */
            break;
        if (toktype == ',') {
            JSON_EXPECT(session, ins, ',');
            if (json_peek(session, ins) != 's')
                goto err;
            continue;
        }
    }
    JSON_EXPECT(session, ins, 0);

    if (0) {
err:
        if (ret == 0)
            ret = EINVAL;
    }
    config_list_free(&cl);
    free(tableuri);
    return (ret);
}

/*
 * json_peek --
 *     Set the input state to the next available token in the input and return its token type, a
 *     code defined by __wt_json_token().
 */
static int
json_peek(WT_SESSION *session, JSON_INPUT_STATE *ins)
{
    WT_DECL_RET;

    if (!ins->peeking) {
        while (!ins->ateof) {
            while (__wt_isspace((u_char)*ins->p))
                ins->p++;
            if (*ins->p)
                break;
            if (ins->kvraw != NULL) {
                if (json_kvraw_append(session, ins, (char *)ins->line.mem + ins->kvrawstart,
                      strlen(ins->line.mem) - ins->kvrawstart)) {
                    ret = -1;
                    goto err;
                }
                ins->kvrawstart = 0;
            }
            if (util_read_line(session, &ins->line, true, &ins->ateof)) {
                ins->toktype = -1;
                ret = -1;
                goto err;
            }
            ins->linenum++;
            ins->p = (const char *)ins->line.mem;
        }
        if (ins->ateof)
            ins->toktype = 0;
        else if (__wt_json_token(session, ins->p, &ins->toktype, &ins->tokstart, &ins->toklen) != 0)
            ins->toktype = -1;
        ins->peeking = true;
    }
    if (0) {
err:
        if (ret == 0)
            ret = -1;
    }
    return (ret == 0 ? ins->toktype : -1);
}

/*
 * json_expect --
 *     Ensure that the type of the next token in the input matches the wanted value, and advance
 *     past it. The values of the input state will be set so specific string or integer values can
 *     be pulled out after this call.
 */
static int
json_expect(WT_SESSION *session, JSON_INPUT_STATE *ins, int wanttok)
{
    if (json_peek(session, ins) < 0)
        return (1);
    ins->p += ins->toklen;
    ins->peeking = false;
    if (ins->toktype != wanttok) {
        fprintf(stderr, "%s: %d: %" WT_SIZET_FMT ": expected %s, got %s\n", ins->filename,
          ins->linenum, JSON_INPUT_POS(ins) + 1, __wt_json_tokname(wanttok),
          __wt_json_tokname(ins->toktype));
        return (1);
    }
    return (0);
}

/*
 * json_skip --
 *     Skip over JSON input until one of the specified strings appears. The tokenizer will be set to
 *     point to the beginning of that string.
 */
static int
json_skip(WT_SESSION *session, JSON_INPUT_STATE *ins, const char **matches)
{
    const char *hit;
    const char **match;

    WT_ASSERT((WT_SESSION_IMPL *)session, ins->kvraw == NULL);
    hit = NULL;
    while (!ins->ateof) {
        for (match = matches; *match != NULL; match++)
            if ((hit = strstr(ins->p, *match)) != NULL)
                goto out;
        if (util_read_line(session, &ins->line, true, &ins->ateof) != 0) {
            ins->toktype = -1;
            return (1);
        }
        ins->linenum++;
        ins->p = (const char *)ins->line.mem;
    }
out:
    if (hit == NULL)
        return (1);

    /* Set to this token. */
    ins->p = hit;
    ins->peeking = false;
    ins->toktype = 0;
    (void)json_peek(session, ins);
    return (0);
}

/*
 * load_json --
 *     Load from the JSON format produced by 'wt dump -j'.
 */
/*
 * util_load_json --
 *     TODO: Add a comment describing this function.
 */
int
util_load_json(WT_SESSION *session, const char *filename, uint32_t flags)
{
    JSON_INPUT_STATE instate;
    WT_DECL_RET;

    memset(&instate, 0, sizeof(instate));
    instate.session = session;
    if ((ret = util_read_line(session, &instate.line, false, &instate.ateof)) == 0) {
        instate.p = (const char *)instate.line.mem;
        instate.linenum = 1;
        instate.filename = filename;

        ret = json_top_level(session, &instate, flags);
    }

    free(instate.line.mem);
    free(instate.kvraw);
    return (ret);
}
