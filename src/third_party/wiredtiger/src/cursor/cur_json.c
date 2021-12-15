/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __json_unpack_put(
  WT_SESSION_IMPL *, void *, u_char *, size_t, WT_CONFIG_ITEM *, size_t *);
static inline int __json_struct_size(
  WT_SESSION_IMPL *, const void *, size_t, const char *, WT_CONFIG_ITEM *, bool, size_t *);
static inline int __json_struct_unpackv(WT_SESSION_IMPL *, const void *, size_t, const char *,
  WT_CONFIG_ITEM *, u_char *, size_t, bool, va_list);
static int json_string_arg(WT_SESSION_IMPL *, const char **, WT_ITEM *);
static int json_int_arg(WT_SESSION_IMPL *, const char **, int64_t *);
static int json_uint_arg(WT_SESSION_IMPL *, const char **, uint64_t *);
static int __json_pack_struct(WT_SESSION_IMPL *, void *, size_t, const char *, const char *);
static int __json_pack_size(
  WT_SESSION_IMPL *, const char *, WT_CONFIG_ITEM *, bool, const char *, size_t *);

#define WT_PACK_JSON_GET(session, pv, jstr)                          \
    do {                                                             \
        switch ((pv).type) {                                         \
        case 'x':                                                    \
            break;                                                   \
        case 's':                                                    \
        case 'S':                                                    \
            WT_RET(json_string_arg(session, &(jstr), &(pv).u.item)); \
            (pv).type = (pv).type == 's' ? 'j' : 'J';                \
            break;                                                   \
        case 'b':                                                    \
        case 'h':                                                    \
        case 'i':                                                    \
        case 'l':                                                    \
        case 'q':                                                    \
            WT_RET(json_int_arg(session, &(jstr), &(pv).u.i));       \
            break;                                                   \
        case 'B':                                                    \
        case 'H':                                                    \
        case 'I':                                                    \
        case 'L':                                                    \
        case 'Q':                                                    \
        case 'r':                                                    \
        case 'R':                                                    \
        case 't':                                                    \
            WT_RET(json_uint_arg(session, &(jstr), &(pv).u.u));      \
            break;                                                   \
        case 'u':                                                    \
            WT_RET(json_string_arg(session, &(jstr), &(pv).u.item)); \
            (pv).type = 'K';                                         \
            break;                                                   \
        default:                                                     \
            /* User format strings have already been validated. */   \
            return (__wt_illegal_value(session, (pv).type));         \
        }                                                            \
    } while (0)

/*
 * __json_unpack_put --
 *     Calculate the size of a packed byte string as formatted for JSON.
 */
static int
__json_unpack_put(WT_SESSION_IMPL *session, void *voidpv, u_char *buf, size_t bufsz,
  WT_CONFIG_ITEM *name, size_t *retsizep)
{
    WT_PACK_VALUE *pv;
    size_t s, n;
    const u_char *p, *end;

    pv = (WT_PACK_VALUE *)voidpv;

    WT_RET(__wt_snprintf_len_set((char *)buf, bufsz, &s, "\"%.*s\" : ", (int)name->len, name->str));
    if (s <= bufsz) {
        bufsz -= s;
        buf += s;
    } else
        bufsz = 0;

    switch (pv->type) {
    case 'x':
        return (0);
    case 's':
    case 'S':
        /* Account for '"' quote in front and back. */
        s += 2;
        p = (const u_char *)pv->u.s;
        if (bufsz > 0) {
            *buf++ = '"';
            bufsz--;
        }
        if (pv->type == 's' || pv->havesize) {
            end = p + pv->size;
            for (; p < end; p++) {
                n = __wt_json_unpack_char(*p, buf, bufsz, false);
                if (n > bufsz)
                    bufsz = 0;
                else {
                    bufsz -= n;
                    buf += n;
                }
                s += n;
            }
        } else
            for (; *p; p++) {
                n = __wt_json_unpack_char(*p, buf, bufsz, false);
                if (n > bufsz)
                    bufsz = 0;
                else {
                    bufsz -= n;
                    buf += n;
                }
                s += n;
            }
        if (bufsz > 0)
            *buf++ = '"';
        *retsizep += s;
        return (0);
    case 'U':
    case 'u':
        s += 2;
        p = (const u_char *)pv->u.item.data;
        end = p + pv->u.item.size;
        if (bufsz > 0) {
            *buf++ = '"';
            bufsz--;
        }
        for (; p < end; p++) {
            n = __wt_json_unpack_char(*p, buf, bufsz, true);
            if (n > bufsz)
                bufsz = 0;
            else {
                bufsz -= n;
                buf += n;
            }
            s += n;
        }
        if (bufsz > 0)
            *buf++ = '"';
        *retsizep += s;
        return (0);
    case 'b':
    case 'h':
    case 'i':
    case 'l':
    case 'q':
        WT_RET(__wt_snprintf_len_incr((char *)buf, bufsz, &s, "%" PRId64, pv->u.i));
        *retsizep += s;
        return (0);
    case 'B':
    case 't':
    case 'H':
    case 'I':
    case 'L':
    case 'Q':
    case 'r':
    case 'R':
        WT_RET(__wt_snprintf_len_incr((char *)buf, bufsz, &s, "%" PRIu64, pv->u.u));
        *retsizep += s;
        return (0);
    }

    WT_RET_MSG(session, EINVAL, "unknown pack-value type: %c", (int)pv->type);
}

/*
 * __json_struct_size --
 *     Calculate the size of a packed byte string as formatted for JSON.
 */
static inline int
__json_struct_size(WT_SESSION_IMPL *session, const void *buffer, size_t size, const char *fmt,
  WT_CONFIG_ITEM *names, bool iskey, size_t *presult)
{
    WT_CONFIG_ITEM name;
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_RET;
    WT_PACK pack;
    WT_PACK_NAME packname;
    size_t result;
    const uint8_t *p, *end;
    bool needcr;

    p = buffer;
    end = p + size;
    result = 0;
    needcr = false;

    __pack_name_init(session, names, iskey, &packname);
    WT_RET(__pack_init(session, &pack, fmt));
    while ((ret = __pack_next(&pack, &pv)) == 0) {
        if (needcr)
            result += 2;
        needcr = true;
        WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));
        WT_RET(__pack_name_next(&packname, &name));
        WT_RET(__json_unpack_put(session, &pv, NULL, 0, &name, &result));
    }
    WT_RET_NOTFOUND_OK(ret);

    /* Be paranoid - __pack_write should never overflow. */
    WT_ASSERT(session, p <= end);

    *presult = result;
    return (0);
}

/*
 * __json_struct_unpackv --
 *     Unpack a byte string to JSON (va_list version).
 */
static inline int
__json_struct_unpackv(WT_SESSION_IMPL *session, const void *buffer, size_t size, const char *fmt,
  WT_CONFIG_ITEM *names, u_char *jbuf, size_t jbufsize, bool iskey, va_list ap)
{
    WT_CONFIG_ITEM name;
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_RET;
    WT_PACK pack;
    WT_PACK_NAME packname;
    size_t jsize;
    const uint8_t *p, *end;
    bool needcr;

    p = buffer;
    end = p + size;
    needcr = false;

    /* Unpacking a cursor marked as json implies a single arg. */
    *va_arg(ap, const char **) = (char *)jbuf;

    __pack_name_init(session, names, iskey, &packname);
    WT_RET(__pack_init(session, &pack, fmt));
    while ((ret = __pack_next(&pack, &pv)) == 0) {
        if (needcr) {
            WT_ASSERT(session, jbufsize >= 3);
            strncat((char *)jbuf, ",\n", jbufsize);
            jbuf += 2;
            jbufsize -= 2;
        }
        needcr = true;
        WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));
        WT_RET(__pack_name_next(&packname, &name));
        jsize = 0;
        WT_RET(__json_unpack_put(session, (u_char *)&pv, jbuf, jbufsize, &name, &jsize));
        WT_ASSERT(session, jsize <= jbufsize);
        jbuf += jsize;
        jbufsize -= jsize;
    }
    WT_RET_NOTFOUND_OK(ret);

    /* Be paranoid - __unpack_read should never overflow. */
    WT_ASSERT(session, p <= end);

    WT_ASSERT(session, jbufsize == 1);

    return (0);
}

/*
 * __wt_json_alloc_unpack --
 *     Allocate space for, and unpack an entry into JSON format.
 */
int
__wt_json_alloc_unpack(WT_SESSION_IMPL *session, const void *buffer, size_t size, const char *fmt,
  WT_CURSOR_JSON *json, bool iskey, va_list ap)
{
    WT_CONFIG_ITEM *names;
    size_t needed;
    char **json_bufp;

    if (iskey) {
        names = &json->key_names;
        json_bufp = &json->key_buf;
    } else {
        names = &json->value_names;
        json_bufp = &json->value_buf;
    }
    needed = 0;
    WT_RET(__json_struct_size(session, buffer, size, fmt, names, iskey, &needed));
    WT_RET(__wt_realloc(session, NULL, needed + 1, json_bufp));
    WT_RET(__json_struct_unpackv(
      session, buffer, size, fmt, names, (u_char *)*json_bufp, needed + 1, iskey, ap));

    return (0);
}

/*
 * __wt_json_close --
 *     Release any json related resources.
 */
void
__wt_json_close(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
    WT_CURSOR_JSON *json;

    if ((json = (WT_CURSOR_JSON *)cursor->json_private) != NULL) {
        __wt_free(session, json->key_buf);
        __wt_free(session, json->value_buf);
        __wt_free(session, json->key_names.str);
        __wt_free(session, json->value_names.str);
        __wt_free(session, json);
    }
}

/*
 * __wt_json_unpack_char --
 *     Unpack a single character into JSON escaped format. Can be called with NULL buf for sizing,
 *     and won't overwrite the buffer end in any case.
 */
size_t
__wt_json_unpack_char(u_char ch, u_char *buf, size_t bufsz, bool force_unicode)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    u_char abbrev;

    if (!force_unicode) {
        /* We treat all non-ASCII characters as non-printable. */
        if (__wt_isascii(ch) && __wt_isprint(ch) && ch != '\\' && ch != '"') {
            if (bufsz >= 1)
                *buf = ch;
            return (1);
        }
        abbrev = '\0';
        switch (ch) {
        case '\\':
        case '"':
            abbrev = ch;
            break;
        case '\f':
            abbrev = 'f';
            break;
        case '\n':
            abbrev = 'n';
            break;
        case '\r':
            abbrev = 'r';
            break;
        case '\t':
            abbrev = 't';
            break;
        }
        if (abbrev != '\0') {
            if (bufsz >= 2) {
                *buf++ = '\\';
                *buf = abbrev;
            }
            return (2);
        }
    }
    if (bufsz >= 6) {
        *buf++ = '\\';
        *buf++ = 'u';
        *buf++ = '0';
        *buf++ = '0';
        *buf++ = __wt_hex((ch & 0xf0) >> 4);
        *buf++ = __wt_hex(ch & 0x0f);
    }
    return (6);
}

/*
 * __wt_json_unpack_str --
 *     Unpack a string into JSON escaped format. Can be called with NULL buf for sizing and won't
 *     overwrite the buffer end in any case.
 */
size_t
__wt_json_unpack_str(u_char *dest, size_t dest_len, const u_char *src, size_t src_len)
{
    size_t n, total;

    for (total = 0; src_len > 0; ++src, --src_len, total += n) {
        n = __wt_json_unpack_char(*src, dest, dest_len, false);
        if (dest_len >= n) {
            dest_len -= n;
            dest += n;
        }
    }
    if (dest_len > 0)
        *dest = '\0';
    return (total);
}

/*
 * __wt_json_column_init --
 *     Set json_key_names, json_value_names to comma separated lists of column names.
 */
int
__wt_json_column_init(WT_CURSOR *cursor, const char *uri, const char *keyformat,
  const WT_CONFIG_ITEM *idxconf, const WT_CONFIG_ITEM *colconf)
{
    WT_CURSOR_JSON *json;
    WT_SESSION_IMPL *session;
    size_t len;
    uint32_t keycnt, nkeys;
    const char *beginkey, *end, *lparen, *p;

    json = (WT_CURSOR_JSON *)cursor->json_private;
    session = CUR2S(cursor);
    beginkey = colconf->str;
    end = beginkey + colconf->len;

    if (idxconf != NULL) {
        len = idxconf->len;
        WT_RET(__wt_strndup(session, idxconf->str, len, &json->key_names.str));
        json->key_names.len = len;
    } else if (colconf->len > 0 && *beginkey == '(') {
        beginkey++;
        if (end[-1] == ')')
            end--;
    }

    for (nkeys = 0; *keyformat; keyformat++)
        if (!__wt_isdigit((u_char)*keyformat))
            nkeys++;

    p = beginkey;
    keycnt = 0;
    while (p < end && keycnt < nkeys) {
        if (*p == ',')
            keycnt++;
        p++;
    }
    if ((lparen = strchr(uri, '(')) != NULL) {
        /* This cursor is a projection. */
        len = strlen(lparen) - 1;
        WT_ASSERT(session, lparen[len] == ')');
        WT_RET(__wt_strndup(session, lparen, len, &json->value_names.str));
        json->value_names.len = len;
    } else {
        len = WT_PTRDIFF(end, p);
        WT_RET(__wt_strndup(session, p, len, &json->value_names.str));
        json->value_names.len = len;
    }
    if (idxconf == NULL) {
        if (p > beginkey)
            p--;
        len = WT_PTRDIFF(p, beginkey);
        WT_RET(__wt_strndup(session, beginkey, len, &json->key_names.str));
        json->key_names.len = len;
    }
    return (0);
}

#define MATCH_KEYWORD(session, in, result, keyword, matchval)                               \
    do {                                                                                    \
        size_t _kwlen = strlen(keyword);                                                    \
        if (strncmp(in, keyword, _kwlen) == 0 && !__wt_isalnum((u_char)(in)[_kwlen])) {     \
            (in) += _kwlen;                                                                 \
            (result) = matchval;                                                            \
        } else {                                                                            \
            const char *_bad = (in);                                                        \
            while (__wt_isalnum((u_char) * (in)))                                           \
                (in)++;                                                                     \
            WT_RET_MSG(                                                                     \
              session, EINVAL, "unknown keyword \"%.*s\" in JSON", (int)((in)-_bad), _bad); \
        }                                                                                   \
    } while (0)

/*
 * __wt_json_token --
 *     Return the type, start position and length of the next JSON token in the input. String tokens
 *     include the quotes. JSON can be entirely parsed using calls to this tokenizer, each call
 *     using a src pointer that is the previously returned tokstart + toklen. The token type
 *     returned is one of: 0 : EOF 's' : string 'i' : intnum 'f' : floatnum ':' : colon ',' : comma
 *     '{' : lbrace '}' : rbrace '[' : lbracket ']' : rbracket 'N' : null 'T' : true 'F' : false
 */
int
__wt_json_token(WT_SESSION *wt_session, const char *src, int *toktype, const char **tokstart,
  size_t *toklen) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_SESSION_IMPL *session;
    int result;
    char ch;
    const char *bad;
    bool backslash, isalph, isfloat;

    result = -1;
    session = (WT_SESSION_IMPL *)wt_session;
    while (__wt_isspace((u_char)*src))
        src++;
    *tokstart = src;

    if (*src == '\0') {
        *toktype = 0;
        *toklen = 0;
        return (0);
    }

    /* JSON is specified in RFC 4627. */
    switch (*src) {
    case '"':
        backslash = false;
        src++;
        while ((ch = *src) != '\0') {
            if (!backslash) {
                if (ch == '"') {
                    src++;
                    result = 's';
                    break;
                }
                if (ch == '\\')
                    backslash = true;
            } else {
                /* We validate Unicode on this pass. */
                if (ch == 'u') {
                    u_char ignored;
                    const u_char *uc;

                    uc = (const u_char *)src;
                    if (__wt_hex2byte(&uc[1], &ignored) || __wt_hex2byte(&uc[3], &ignored))
                        WT_RET_MSG(session, EINVAL, "invalid Unicode within JSON string");
                    src += 4;
                }
                backslash = false;
            }
            src++;
        }
        if (result == 's')
            break;
        WT_RET_MSG(session, EINVAL, "unterminated string in JSON");
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        isfloat = false;
        if (*src == '-')
            src++;
        while ((ch = *src) != '\0' && __wt_isdigit((u_char)ch))
            src++;
        if (*src == '.') {
            isfloat = true;
            src++;
            while ((ch = *src) != '\0' && __wt_isdigit((u_char)ch))
                src++;
        }
        if (*src == 'e' || *src == 'E') {
            isfloat = true;
            src++;
            if (*src == '+' || *src == '-')
                src++;
            while ((ch = *src) != '\0' && __wt_isdigit((u_char)ch))
                src++;
        }
        result = isfloat ? 'f' : 'i';
        break;
    case ':':
    case ',':
    case '{':
    case '}':
    case '[':
    case ']':
        result = *src++;
        break;
    case 'n':
        MATCH_KEYWORD(session, src, result, "null", 'N');
        break;
    case 't':
        MATCH_KEYWORD(session, src, result, "true", 'T');
        break;
    case 'f':
        MATCH_KEYWORD(session, src, result, "false", 'F');
        break;
    default:
        /* An illegal token, move past it anyway */
        bad = src;
        isalph = __wt_isalnum((u_char)*src);
        src++;
        if (isalph)
            while (*src != '\0' && __wt_isalnum((u_char)*src))
                src++;
        WT_RET_MSG(session, EINVAL, "unknown token \"%.*s\" in JSON", (int)(src - bad), bad);
        /* NOTREACHED */
    }
    WT_ASSERT(session, result != -1);

    *toklen = (size_t)(src - *tokstart);
    *toktype = result;
    return (0);
}

/*
 * __wt_json_tokname --
 *     Return a descriptive name from the token type returned by __wt_json_token.
 */
const char *
__wt_json_tokname(int toktype) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    switch (toktype) {
    case 0:
        return ("<EOF>");
    case 's':
        return ("<string>");
    case 'i':
        return ("<integer>");
    case 'f':
        return ("<float>");
    case ':':
        return ("':'");
    case ',':
        return ("','");
    case '{':
        return ("'{'");
    case '}':
        return ("'}'");
    case '[':
        return ("'['");
    case ']':
        return ("']'");
    case 'N':
        return ("'null'");
    case 'T':
        return ("'true'");
    case 'F':
        return ("'false'");
    default:
        return ("<UNKNOWN>");
    }
}

/*
 * json_string_arg --
 *     Returns a first cut of the needed string in item. The result has not been stripped of
 *     escapes.
 */
static int
json_string_arg(WT_SESSION_IMPL *session, const char **jstr, WT_ITEM *item)
{
    int tok;
    const char *tokstart;

    WT_RET(__wt_json_token((WT_SESSION *)session, *jstr, &tok, &tokstart, &item->size));
    if (tok == 's') {
        *jstr = tokstart + item->size;
        /* The tokenizer includes the '"' chars */
        item->data = tokstart + 1;
        item->size -= 2;
    } else
        WT_RET_MSG(session, EINVAL, "expected JSON <string>, got %s", __wt_json_tokname(tok));
    return (0);
}

/*
 * json_int_arg --
 *     Returns a signed integral value from the current position in the JSON string.
 */
static int
json_int_arg(WT_SESSION_IMPL *session, const char **jstr, int64_t *ip)
{
    size_t toksize;
    int tok;
    char *end;
    const char *tokstart;

    WT_RET(__wt_json_token((WT_SESSION *)session, *jstr, &tok, &tokstart, &toksize));
    if (tok == 'i') {
        /* JSON only allows decimal */
        *ip = strtoll(tokstart, &end, 10);
        if (end != tokstart + toksize)
            WT_RET_MSG(session, EINVAL, "JSON <int> extraneous input");
        *jstr = tokstart + toksize;
    } else
        WT_RET_MSG(session, EINVAL, "expected JSON <int>, got %s", __wt_json_tokname(tok));
    return (0);
}

/*
 * json_uint_arg --
 *     Returns an unsigned integral value from the current position in the JSON string.
 */
static int
json_uint_arg(WT_SESSION_IMPL *session, const char **jstr, uint64_t *up)
{
    size_t toksize;
    int tok;
    char *end;
    const char *tokstart;

    WT_RET(__wt_json_token((WT_SESSION *)session, *jstr, &tok, &tokstart, &toksize));
    if (tok == 'i' && *tokstart != '-') {
        /* JSON only allows decimal */
        *up = strtoull(tokstart, &end, 10);
        if (end != tokstart + toksize)
            WT_RET_MSG(session, EINVAL, "JSON <int> extraneous input");
        *jstr = tokstart + toksize;
    } else
        WT_RET_MSG(session, EINVAL, "expected unsigned JSON <int>, got %s", __wt_json_tokname(tok));
    return (0);
}

#define JSON_EXPECT_TOKEN_GET(session, jstr, tokval, start, sz)                                \
    do {                                                                                       \
        int __tok;                                                                             \
        WT_RET(__wt_json_token((WT_SESSION *)(session), jstr, &__tok, &(start), &(sz)));       \
        if (__tok != (tokval))                                                                 \
            WT_RET_MSG(session, EINVAL, "expected JSON %s, got %s", __wt_json_tokname(tokval), \
              __wt_json_tokname(__tok));                                                       \
        (jstr) = (start) + (sz);                                                               \
    } while (0)

#define JSON_EXPECT_TOKEN(session, jstr, tokval)                     \
    do {                                                             \
        const char *__start;                                         \
        size_t __sz;                                                 \
        JSON_EXPECT_TOKEN_GET(session, jstr, tokval, __start, __sz); \
    } while (0)

/*
 * __json_pack_struct --
 *     Pack a byte string from a JSON string.
 */
static int
__json_pack_struct(
  WT_SESSION_IMPL *session, void *buffer, size_t size, const char *fmt, const char *jstr)
{
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_RET;
    WT_PACK pack;
    size_t toksize;
    uint8_t *p, *end;
    const char *tokstart;
    bool multi;

    p = buffer;
    end = p + size;
    multi = false;

    if (fmt[0] != '\0' && fmt[1] == '\0') {
        JSON_EXPECT_TOKEN_GET(session, jstr, 's', tokstart, toksize);
        /* the key name was verified in __json_pack_size */
        JSON_EXPECT_TOKEN(session, jstr, ':');
        pv.type = fmt[0];
        WT_PACK_JSON_GET(session, pv, jstr);
        return (__pack_write(session, &pv, &p, size));
    }

    WT_RET(__pack_init(session, &pack, fmt));
    while ((ret = __pack_next(&pack, &pv)) == 0) {
        if (multi)
            JSON_EXPECT_TOKEN(session, jstr, ',');
        JSON_EXPECT_TOKEN_GET(session, jstr, 's', tokstart, toksize);
        /* the key name was verified in __json_pack_size */
        JSON_EXPECT_TOKEN(session, jstr, ':');
        WT_PACK_JSON_GET(session, pv, jstr);
        WT_RET(__pack_write(session, &pv, &p, (size_t)(end - p)));
        multi = true;
    }
    WT_RET_NOTFOUND_OK(ret);

    /* Be paranoid - __pack_write should never overflow. */
    WT_ASSERT(session, p <= end);

    return (0);
}

/*
 * __json_pack_size --
 *     Calculate the size of a packed byte string from a JSON string. We verify that the names and
 *     value types provided in JSON match the column names and type from the schema format,
 *     returning error if not.
 */
static int
__json_pack_size(WT_SESSION_IMPL *session, const char *fmt, WT_CONFIG_ITEM *names, bool iskey,
  const char *jstr, size_t *sizep)
{
    WT_CONFIG_ITEM name;
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_RET;
    WT_PACK pack;
    WT_PACK_NAME packname;
    size_t toksize, v;
    const char *tokstart;
    bool multi;

    __pack_name_init(session, names, iskey, &packname);
    multi = false;
    WT_RET(__pack_init(session, &pack, fmt));
    for (*sizep = 0; (ret = __pack_next(&pack, &pv)) == 0;) {
        if (multi)
            JSON_EXPECT_TOKEN(session, jstr, ',');
        JSON_EXPECT_TOKEN_GET(session, jstr, 's', tokstart, toksize);
        WT_RET(__pack_name_next(&packname, &name));
        if (toksize - 2 != name.len || strncmp(tokstart + 1, name.str, toksize - 2) != 0)
            WT_RET_MSG(session, EINVAL, "JSON expected %s name: \"%.*s\"", iskey ? "key" : "value",
              (int)name.len, name.str);
        JSON_EXPECT_TOKEN(session, jstr, ':');
        WT_PACK_JSON_GET(session, pv, jstr);
        WT_RET(__pack_size(session, &pv, &v));
        *sizep += v;
        multi = true;
    }
    WT_RET_NOTFOUND_OK(ret);

    /* check end of string */
    JSON_EXPECT_TOKEN(session, jstr, 0);

    return (0);
}

/*
 * __wt_json_to_item --
 *     Convert a JSON input string for either key/value to a raw WT_ITEM. Checks that the input
 *     matches the expected format.
 */
int
__wt_json_to_item(WT_SESSION_IMPL *session, const char *jstr, const char *format,
  WT_CURSOR_JSON *json, bool iskey, WT_ITEM *item)
{
    size_t sz;
    sz = 0; /* Initialize because GCC 4.1 is paranoid */

    WT_RET(__json_pack_size(
      session, format, iskey ? &json->key_names : &json->value_names, iskey, jstr, &sz));
    WT_RET(__wt_buf_initsize(session, item, sz));
    WT_RET(__json_pack_struct(session, item->mem, sz, format, jstr));
    return (0);
}

/*
 * __wt_json_strlen --
 *     Return the number of bytes represented by a string in JSON format, or -1 if the format is
 *     incorrect.
 */
ssize_t
__wt_json_strlen(const char *src, size_t srclen) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    size_t dstlen;
    u_char hi, lo;
    const char *srcend;

    dstlen = 0;
    srcend = src + srclen;
    while (src < srcend) {
        /* JSON can include any UTF-8 expressed in 4 hex chars. */
        if (*src == '\\') {
            if (*++src == 'u') {
                if (__wt_hex2byte((const u_char *)++src, &hi))
                    return (-1);
                src += 2;
                if (__wt_hex2byte((const u_char *)src, &lo))
                    return (-1);
                src += 2;
                if (hi != 0)
                    /*
                     * For our dump representation, every Unicode character on input represents a
                     * single byte.
                     */
                    return (-1);
            }
        } else
            src++;
        dstlen++;
    }
    if (src != srcend)
        return (-1); /* invalid input, e.g. final char is '\\' */
    return ((ssize_t)dstlen);
}

/*
 * __wt_json_strncpy --
 *     Copy bytes of string in JSON format to a destination, up to dstlen bytes. If dstlen is
 *     greater than the needed size, the result if zero padded.
 */
int
__wt_json_strncpy(WT_SESSION *wt_session, char **pdst, size_t dstlen, const char *src,
  size_t srclen) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_SESSION_IMPL *session;
    u_char hi, lo;
    char ch, *dst;
    const char *dstend, *srcend;

    session = (WT_SESSION_IMPL *)wt_session;

    dst = *pdst;
    dstend = dst + dstlen;
    srcend = src + srclen;
    while (src < srcend && dst < dstend) {
        /* JSON can include any UTF-8 expressed in 4 hex chars. */
        if ((ch = *src++) == '\\')
            switch (ch = *src++) {
            case 'u':
                if (__wt_hex2byte((const u_char *)src, &hi) ||
                  __wt_hex2byte((const u_char *)src + 2, &lo))
                    WT_RET_MSG(session, EINVAL, "invalid Unicode within JSON string");
                src += 4;
                if (hi != 0)
                    WT_RET_MSG(
                      session, EINVAL, "Unicode \"%6.6s\" byte out of range in JSON", src - 6);
                *dst++ = (char)lo;
                break;
            case 'f':
                *dst++ = '\f';
                break;
            case 'n':
                *dst++ = '\n';
                break;
            case 'r':
                *dst++ = '\r';
                break;
            case 't':
                *dst++ = '\t';
                break;
            case '"':
            case '\\':
                *dst++ = ch;
                break;
            default:
                return (__wt_illegal_value(session, ch));
            }
        else
            *dst++ = ch;
    }
    if (src != srcend)
        WT_RET_MSG(session, ENOMEM, "JSON string copy destination buffer too small");
    *pdst = dst;
    while (dst < dstend)
        *dst++ = '\0';
    return (0);
}
