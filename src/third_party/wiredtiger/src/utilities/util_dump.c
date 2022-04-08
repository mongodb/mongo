/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <assert.h>
#include "util.h"
#include "util_dump.h"

#define STRING_MATCH_CONFIG(s, item) \
    (strncmp(s, (item).str, (item).len) == 0 && (s)[(item).len] == '\0')

static int dump_config(WT_SESSION *, const char *, WT_CURSOR *, bool, bool, bool);
static int dump_json_begin(WT_SESSION *);
static int dump_json_end(WT_SESSION *);
static int dump_json_separator(WT_SESSION *);
static int dump_json_table_end(WT_SESSION *);
static const char *get_dump_type(bool, bool, bool);
static int dump_prefix(WT_SESSION *, bool, bool, bool);
static int dump_record(WT_CURSOR *, bool, bool);
static int dump_suffix(WT_SESSION *, bool);
static int dump_table_config(WT_SESSION *, WT_CURSOR *, WT_CURSOR *, const char *, bool);
static int dump_table_parts_config(WT_SESSION *, WT_CURSOR *, const char *, const char *, bool);
static int dup_json_string(const char *, char **);
static int print_config(WT_SESSION *, const char *, const char *, bool, bool);
static int time_pair_to_timestamp(WT_SESSION_IMPL *, char *, WT_ITEM *);

/*
 * usage --
 *     Display a usage message for the dump command.
 */
static int
usage(void)
{
    static const char *options[] = {"-c checkpoint",
      "dump as of the named checkpoint (the default is the most recent version of the data)",
      "-f output", "dump to the specified file (the default is stdout)", "-j",
      "dump in JSON format", "-p",
      "dump in human readable format (pretty-print). The -p flag can be combined with -x. In this "
      "case, raw data elements will be formatted like -x with hexadecimal encoding.",
      "-r", "dump in reverse order", "-t timestamp",
      "dump as of the specified timestamp (the default is the most recent version of the data)",
      "-x",
      "dump all characters in a hexadecimal encoding (by default printable characters are not "
      "encoded). The -x flag can be combined with -p. In this case, the dump will be formatted "
      "similar to -p except for raw data elements, which will look like -x with hexadecimal "
      "encoding.",
      NULL, NULL};

    util_usage(
      "dump [-jprx] [-c checkpoint] [-f output-file] [-t timestamp] uri", "options:", options);
    return (1);
}

static FILE *fp;

/*
 * util_dump --
 *     The dump command.
 */
int
util_dump(WT_SESSION *session, int argc, char *argv[])
{
    WT_CURSOR *cursor;
    WT_CURSOR_DUMP *hs_dump_cursor;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_SESSION_IMPL *session_impl;
    int ch, format_specifiers, i;
    char *checkpoint, *ofile, *p, *simpleuri, *timestamp, *uri;
    bool hex, json, pretty, reverse;

    session_impl = (WT_SESSION_IMPL *)session;

    cursor = NULL;
    hs_dump_cursor = NULL;
    checkpoint = ofile = simpleuri = uri = timestamp = NULL;
    hex = json = pretty = reverse = false;
    while ((ch = __wt_getopt(progname, argc, argv, "c:f:t:jprx")) != EOF)
        switch (ch) {
        case 'c':
            checkpoint = __wt_optarg;
            break;
        case 'f':
            ofile = __wt_optarg;
            break;
        case 'j':
            json = true;
            break;
        case 'p':
            pretty = true;
            break;
        case 'r':
            reverse = true;
            break;
        case 't':
            timestamp = __wt_optarg;
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

    /* The remaining argument is the uri. */
    if (argc < 1 || (argc != 1 && !json))
        return (usage());

    /* -j, -p and -x are incompatible. */
    format_specifiers = 0;
    if (json)
        ++format_specifiers;
    if (pretty)
        ++format_specifiers;
    if (hex)
        ++format_specifiers;

    /* Supported options are -j, -p, -x and a combination of -p and -x. */
    if (format_specifiers > 1 && !(pretty && hex)) {
        fprintf(stderr,
          "%s: the only possible options are -j, -p, -x and a combination of -p and -x. Other "
          "options are incompatible\n",
          progname);
        return (usage());
    }

    /* Open any optional output file. */
    if (ofile == NULL)
        fp = stdout;
    else if ((fp = fopen(ofile, "w")) == NULL)
        return (util_err(session, errno, "%s: open", ofile));

    if (json && (dump_json_begin(session) != 0 || dump_prefix(session, pretty, hex, json) != 0))
        goto err;

    WT_RET(__wt_scr_alloc(session_impl, 0, &tmp));
    for (i = 0; i < argc; i++) {
        if (json && i > 0)
            if (dump_json_separator(session) != 0)
                goto err;
        free(uri);
        free(simpleuri);
        uri = simpleuri = NULL;

        if ((uri = util_uri(session, argv[i], "table")) == NULL)
            goto err;

        if (timestamp != NULL) {
            WT_ERR(__wt_buf_set(session_impl, tmp, "", 0));
            WT_ERR(time_pair_to_timestamp(session_impl, timestamp, tmp));
            WT_ERR(__wt_buf_catfmt(session_impl, tmp, "isolation=snapshot,"));
            if ((ret = session->begin_transaction(session, (char *)tmp->data)) != 0) {
                fprintf(stderr, "%s: begin transaction failed: %s\n", progname,
                  session->strerror(session, ret));
                goto err;
            }
        }
        WT_ERR(__wt_buf_set(session_impl, tmp, "", 0));
        if (checkpoint != NULL)
            WT_ERR(__wt_buf_catfmt(session_impl, tmp, "checkpoint=%s,", checkpoint));
        WT_ERR(__wt_buf_catfmt(session_impl, tmp, "dump=%s", get_dump_type(pretty, hex, json)));
        if ((ret = session->open_cursor(session, uri, NULL, (char *)tmp->data, &cursor)) != 0) {
            fprintf(stderr, "%s: cursor open(%s) failed: %s\n", progname, uri,
              session->strerror(session, ret));
            goto err;
        }

        if ((simpleuri = strdup(uri)) == NULL) {
            (void)util_err(session, errno, NULL);
            goto err;
        }
        if ((p = strchr(simpleuri, '(')) != NULL)
            *p = '\0';
        /*
         * If we're dumping the history store, we need to set this flag to ignore tombstones. Every
         * record in the history store is succeeded by a tombstone so we need to do this otherwise
         * nothing will be visible. The only exception is if we've supplied a timestamp in which
         * case, we're specifically interested in what is visible at a given read timestamp.
         */
        if (WT_STREQ(simpleuri, WT_HS_URI) && timestamp == NULL) {
            hs_dump_cursor = (WT_CURSOR_DUMP *)cursor;
            /* Set the "ignore tombstone" flag on the underlying cursor. */
            F_SET(hs_dump_cursor->child, WT_CURSTD_IGNORE_TOMBSTONE);
        }
        if (dump_config(session, simpleuri, cursor, pretty, hex, json) != 0)
            goto err;

        if (dump_record(cursor, reverse, json) != 0)
            goto err;
        if (json && dump_json_table_end(session) != 0)
            goto err;

        if (hs_dump_cursor != NULL)
            F_CLR(hs_dump_cursor->child, WT_CURSTD_IGNORE_TOMBSTONE);
        ret = cursor->close(cursor);
        cursor = NULL;
        hs_dump_cursor = NULL;
        if (ret != 0) {
            (void)util_err(session, ret, NULL);
            goto err;
        }
    }
    if (json && dump_json_end(session) != 0)
        goto err;

    if (0) {
err:
        ret = 1;
    }

    if (cursor != NULL) {
        if (hs_dump_cursor != NULL)
            F_CLR(hs_dump_cursor->child, WT_CURSTD_IGNORE_TOMBSTONE);
        if ((ret = cursor->close(cursor)) != 0)
            ret = util_err(session, ret, NULL);
    }

    if (ofile != NULL && (ret = fclose(fp)) != 0)
        ret = util_err(session, errno, NULL);

    __wt_scr_free(session_impl, &tmp);
    free(uri);
    free(simpleuri);

    return (ret);
}

/*
 * time_pair_to_timestamp --
 *     Convert a timestamp output format to timestamp representation.
 */
static int
time_pair_to_timestamp(WT_SESSION_IMPL *session_impl, char *ts_string, WT_ITEM *buf)
{
    wt_timestamp_t timestamp;
    uint32_t first, second;

    if (ts_string[0] == '(') {
        if (sscanf(ts_string, "(%" SCNu32 " ,%" SCNu32 ")", &first, &second) != 2)
            return (EINVAL);
        timestamp = ((wt_timestamp_t)first << 32) | second;
    } else
        timestamp = __wt_strtouq(ts_string, NULL, 10);

    WT_RET(__wt_buf_catfmt(session_impl, buf, "read_timestamp=%" PRIx64 ",", timestamp));
    return (0);
}

/*
 * dump_config --
 *     Dump the config for the uri.
 */
static int
dump_config(
  WT_SESSION *session, const char *uri, WT_CURSOR *cursor, bool pretty, bool hex, bool json)
{
    WT_CURSOR *mcursor;
    WT_DECL_RET;
    int tret;

    /* Open a metadata cursor. */
    if ((ret = session->open_cursor(session, "metadata:create", NULL, NULL, &mcursor)) != 0) {
        fprintf(stderr, "%s: %s: session.open_cursor: %s\n", progname, "metadata:create",
          session->strerror(session, ret));
        return (1);
    }
    /*
     * Search for the object itself, just to make sure it exists, we don't want to output a header
     * if the user entered the wrong name. This is where we find out a table doesn't exist, use a
     * simple error message.
     */
    mcursor->set_key(mcursor, uri);
    if ((ret = mcursor->search(mcursor)) == 0) {
        if ((!json && dump_prefix(session, pretty, hex, json) != 0) ||
          dump_table_config(session, mcursor, cursor, uri, json) != 0 ||
          dump_suffix(session, json) != 0)
            ret = 1;
    } else if (ret == WT_NOTFOUND)
        ret = util_err(session, 0, "%s: No such object exists", uri);
    else if (ret == ENOTSUP)
        /*
         * Ignore ENOTSUP error. We return that for getting the creation metadata for a complex
         * table because the meaning of that is undefined. It does mean the table exists.
         */
        ret = 0;
    else
        ret = util_err(session, ret, "%s", uri);

    if ((tret = mcursor->close(mcursor)) != 0) {
        tret = util_cerr(mcursor, "close", tret);
        if (ret == 0)
            ret = tret;
    }

    return (ret);
}

/*
 * dump_json_begin --
 *     Output the dump file header prefix.
 */
static int
dump_json_begin(WT_SESSION *session)
{
    if (fprintf(fp, "{\n") < 0)
        return (util_err(session, EIO, NULL));
    return (0);
}

/*
 * dump_json_end --
 *     Output the dump file header suffix.
 */
static int
dump_json_end(WT_SESSION *session)
{
    if (fprintf(fp, "\n}\n") < 0)
        return (util_err(session, EIO, NULL));
    return (0);
}

/*
 * dump_json_separator --
 *     Output a separator between two JSON outputs in a list.
 */
static int
dump_json_separator(WT_SESSION *session)
{
    if (fprintf(fp, ",\n") < 0)
        return (util_err(session, EIO, NULL));
    return (0);
}

/*
 * dump_json_table_end --
 *     Output the JSON syntax that ends a table.
 */
static int
dump_json_table_end(WT_SESSION *session)
{
    if (fprintf(fp, "            ]\n        }\n    ]") < 0)
        return (util_err(session, EIO, NULL));
    return (0);
}

/*
 * dump_add_config --
 *     Add a formatted config string to an output buffer.
 */
static int
dump_add_config(WT_SESSION *session, char **bufp, size_t *leftp, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 4, 5)))
{
    WT_DECL_RET;
    size_t n;
    va_list ap;

    va_start(ap, fmt);
    ret = __wt_vsnprintf_len_set(*bufp, *leftp, &n, fmt, ap);
    va_end(ap);
    if (ret != 0)
        return (util_err(session, ret, NULL));
    *bufp += n;
    *leftp -= n;
    return (0);
}

/*
 * dump_projection --
 *     Create a new config containing projection information.
 */
static int
dump_projection(WT_SESSION *session, const char *config, WT_CURSOR *cursor, char **newconfigp)
{
    WT_CONFIG_ITEM key, value;
    WT_CONFIG_PARSER *parser;
    WT_DECL_RET;
    WT_EXTENSION_API *wt_api;
    size_t len, vallen;
    int nkeys;
    char *newconfig;
    const char *keyformat, *p;

    len = strlen(config) + strlen(cursor->value_format) + strlen(cursor->uri) + 20;
    if ((newconfig = malloc(len)) == NULL)
        return (util_err(session, errno, NULL));
    *newconfigp = newconfig;
    wt_api = session->connection->get_extension_api(session->connection);
    if ((ret = wt_api->config_parser_open(wt_api, session, config, strlen(config), &parser)) != 0)
        return (util_err(session, ret, "WT_EXTENSION_API.config_parser_open"));
    keyformat = cursor->key_format;
    for (nkeys = 0; *keyformat; keyformat++)
        if (!__wt_isdigit((u_char)*keyformat))
            nkeys++;

    /*
     * Copy the configuration, replacing some fields to match the projection.
     */
    while ((ret = parser->next(parser, &key, &value)) == 0) {
        WT_RET(dump_add_config(session, &newconfig, &len, "%.*s=", (int)key.len, key.str));
        if (STRING_MATCH_CONFIG("value_format", key))
            WT_RET(dump_add_config(session, &newconfig, &len, "%s,", cursor->value_format));
        else if (STRING_MATCH_CONFIG("columns", key)) {
            /* copy names of keys */
            p = value.str;
            vallen = value.len;
            while (vallen > 0) {
                if ((*p == ',' || *p == ')') && --nkeys == 0)
                    break;
                p++;
                vallen--;
            }
            WT_RET(
              dump_add_config(session, &newconfig, &len, "%.*s", (int)(p - value.str), value.str));

            /* copy names of projected values */
            p = strchr(cursor->uri, '(');
            assert(p != NULL);
            assert(p[strlen(p) - 1] == ')');
            p++;
            if (*p != ')')
                WT_RET(dump_add_config(session, &newconfig, &len, "%s", ","));
            WT_RET(dump_add_config(session, &newconfig, &len, "%.*s),", (int)(strlen(p) - 1), p));
        } else if (value.type == WT_CONFIG_ITEM_STRING && value.len != 0)
            WT_RET(
              dump_add_config(session, &newconfig, &len, "\"%.*s\",", (int)value.len, value.str));
        else
            WT_RET(dump_add_config(session, &newconfig, &len, "%.*s,", (int)value.len, value.str));
    }
    if (ret != WT_NOTFOUND)
        return (util_err(session, ret, "WT_CONFIG_PARSER.next"));

    assert(len > 0);
    if ((ret = parser->close(parser)) != 0)
        return (util_err(session, ret, "WT_CONFIG_PARSER.close"));

    return (0);
}

/*
 * dump_table_config --
 *     Dump the config for a table.
 */
static int
dump_table_config(
  WT_SESSION *session, WT_CURSOR *mcursor, WT_CURSOR *cursor, const char *uri, bool json)
{
    WT_DECL_RET;
    char *proj_config;
    const char *name, *v;

    proj_config = NULL;
    /* Get the table name. */
    if ((name = strchr(uri, ':')) == NULL) {
        fprintf(stderr, "%s: %s: corrupted uri\n", progname, uri);
        return (1);
    }
    ++name;

    /*
     * Dump out the config information: first, dump the uri entry itself, it overrides all
     * subsequent configurations.
     */
    mcursor->set_key(mcursor, uri);
    if ((ret = mcursor->search(mcursor)) != 0)
        return (util_cerr(mcursor, "search", ret));
    if ((ret = mcursor->get_value(mcursor, &v)) != 0)
        return (util_cerr(mcursor, "get_value", ret));

    if (strchr(cursor->uri, '(') != NULL) {
        WT_ERR(dump_projection(session, v, cursor, &proj_config));
        v = proj_config;
    }
    WT_ERR(print_config(session, uri, v, json, true));

    WT_ERR(dump_table_parts_config(session, mcursor, name, "colgroup:", json));
    WT_ERR(dump_table_parts_config(session, mcursor, name, "index:", json));

err:
    free(proj_config);
    return (ret);
}

/*
 * dump_table_parts_config --
 *     Dump the column groups or indices parts with a table.
 */
static int
dump_table_parts_config(
  WT_SESSION *session, WT_CURSOR *cursor, const char *name, const char *entry, bool json)
{
    WT_DECL_RET;
    size_t len;
    int exact;
    char *uriprefix;
    const char *groupname, *key, *sep;
    const char *v;
    bool multiple;

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
        if (fprintf(fp, "            \"%s\" : [", groupname) < 0)
            return (util_err(session, EIO, NULL));
    }

    len = strlen(entry) + strlen(name) + 1;
    if ((uriprefix = malloc(len)) == NULL)
        return (util_err(session, errno, NULL));
    if ((ret = __wt_snprintf(uriprefix, len, "%s%s", entry, name)) != 0) {
        free(uriprefix);
        return (util_err(session, ret, NULL));
    }

    /*
     * Search the file looking for column group and index key/value pairs: for each one, look up the
     * related source information and append it to the base record, where the column group and index
     * configuration overrides the source configuration.
     */
    cursor->set_key(cursor, uriprefix);
    ret = cursor->search_near(cursor, &exact);
    free(uriprefix);
    if (ret == WT_NOTFOUND)
        return (0);
    if (ret != 0)
        return (util_cerr(cursor, "search_near", ret));

    /*
     * An exact match is only possible for column groups, and indicates there is an implicit
     * (unnamed) column group. Any configuration for such a column group has already been folded
     * into the configuration for the associated table, so it is not interesting.
     */
    if (exact > 0)
        goto match;
    while (exact != 0 && (ret = cursor->next(cursor)) == 0) {
match:
        if ((ret = cursor->get_key(cursor, &key)) != 0)
            return (util_cerr(cursor, "get_key", ret));

        /* Check if we've finished the list of entries. */
        if (!WT_PREFIX_MATCH(key, entry) || !WT_PREFIX_MATCH(key + strlen(entry), name))
            break;

        if ((ret = cursor->get_value(cursor, &v)) != 0)
            return (util_cerr(cursor, "get_value", ret));

        if (json && fprintf(fp, "%s\n", (multiple ? "," : "")) < 0)
            return (util_err(session, EIO, NULL));
        /*
         * The dumped configuration string is the original key plus the source's configuration,
         * where the values of the original key override any source configurations of the same name.
         */
        if (print_config(session, key, v, json, false) != 0)
            return (util_err(session, EIO, NULL));
        multiple = true;
    }
    if (json && fprintf(fp, "%s]%s\n", (multiple ? "\n            " : ""), sep) < 0)
        return (util_err(session, EIO, NULL));

    if (ret == 0 || ret == WT_NOTFOUND)
        return (0);
    return (util_cerr(cursor, "next", ret));
}

/*
 * get_dump_type --
 *     Returns dump type string based on the passed format flags
 */
static const char *
get_dump_type(bool pretty, bool hex, bool json)
{
    const char *result;

    result = NULL;

    if (json)
        result = "json";
    else if (hex && pretty)
        result = "pretty_hex";
    else if (hex)
        result = "hex";
    else if (pretty)
        result = "pretty";
    else
        result = "print";

    return (result);
}

/*
 * dump_prefix --
 *     Output the dump file header prefix.
 */
static int
dump_prefix(WT_SESSION *session, bool pretty, bool hex, bool json)
{
    int vmajor, vminor, vpatch;

    (void)wiredtiger_version(&vmajor, &vminor, &vpatch);

    if (json &&
      fprintf(fp, "    \"%s\" : \"%d (%d.%d.%d)\",\n", DUMP_JSON_VERSION_MARKER,
        DUMP_JSON_CURRENT_VERSION, vmajor, vminor, vpatch) < 0)
        return (util_err(session, EIO, NULL));

    if (!json &&
      (fprintf(fp, "WiredTiger Dump (WiredTiger Version %d.%d.%d)\n", vmajor, vminor, vpatch) < 0 ||
        fprintf(fp, "Format=%s\n", (pretty && hex) ? "print hex" : hex ? "hex" : "print") < 0 ||
        fprintf(fp, "Header\n") < 0))
        return (util_err(session, EIO, NULL));

    return (0);
}

/*
 * dump_record --
 *     Dump a single record, advance cursor to next/prev, along with JSON formatting if needed.
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
    while ((ret = (reverse ? cursor->prev(cursor) : cursor->next(cursor))) == 0) {
        if ((ret = cursor->get_key(cursor, &key)) != 0)
            return (util_cerr(cursor, "get_key", ret));
        if ((ret = cursor->get_value(cursor, &value)) != 0)
            return (util_cerr(cursor, "get_value", ret));
        if (fprintf(
              fp, "%s%s%s%s%s%s", json && once ? "," : "", prefix, key, infix, value, suffix) < 0)
            return (util_err(session, EIO, NULL));
        once = true;
    }
    if (json && once && fprintf(fp, "\n") < 0)
        return (util_err(session, EIO, NULL));
    return (ret == WT_NOTFOUND ? 0 : util_cerr(cursor, (reverse ? "prev" : "next"), ret));
}

/*
 * dump_suffix --
 *     Output the dump file header suffix.
 */
static int
dump_suffix(WT_SESSION *session, bool json)
{
    if (json) {
        if (fprintf(fp,
              "        },\n"
              "        {\n"
              "            \"data\" : [") < 0)
            return (util_err(session, EIO, NULL));
    } else {
        if (fprintf(fp, "Data\n") < 0)
            return (util_err(session, EIO, NULL));
    }
    return (0);
}

/*
 * dup_json_string --
 *     Like strdup, but escape any characters that are special for JSON. The result will be embedded
 *     in a JSON string.
 */
static int
dup_json_string(const char *str, char **result)
{
    size_t nchars;
    char *q;

    nchars = __wt_json_unpack_str(NULL, 0, (const u_char *)str, strlen(str)) + 1;
    q = malloc(nchars);
    if (q == NULL)
        return (1);
    WT_IGNORE_RET(__wt_json_unpack_str((u_char *)q, nchars, (const u_char *)str, strlen(str)));
    *result = q;
    return (0);
}

/*
 * print_config --
 *     Output a key/value URI pair by combining v1 and v2.
 */
static int
print_config(WT_SESSION *session, const char *key, const char *cfg, bool json, bool toplevel)
{
    WT_DECL_RET;
    char *jsonconfig;

    /*
     * We have all of the object configuration, but don't have the default session.create
     * configuration. Have the underlying library add in the defaults and collapse it all into one
     * load configuration string.
     */
    jsonconfig = NULL;
    if (json && (ret = dup_json_string(cfg, &jsonconfig)) != 0)
        return (util_err(session, ret, NULL));

    if (json) {
        if (toplevel)
            ret = fprintf(fp,
              "    \"%s\" : [\n        {\n            "
              "\"config\" : \"%s\",\n",
              key, jsonconfig);
        else
            ret = fprintf(fp,
              "                {\n"
              "                    \"uri\" : \"%s\",\n"
              "                    \"config\" : \"%s\"\n"
              "                }",
              key, jsonconfig);
    } else
        ret = fprintf(fp, "%s\n%s\n", key, cfg);
    free(jsonconfig);
    if (ret < 0)
        return (util_err(session, EIO, NULL));
    return (0);
}
