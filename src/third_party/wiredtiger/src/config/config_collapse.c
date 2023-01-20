/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_config_collapse --
 *     Collapse a set of configuration strings into newly allocated memory. This function takes a
 *     NULL-terminated list of configuration strings (where the first one contains all the defaults
 *     and the values are in order from least to most preferred, that is, the default values are
 *     least preferred), and collapses them into newly allocated memory. The algorithm is to walk
 *     the first of the configuration strings, and for each entry, search all of the configuration
 *     strings for a final value, keeping the last value found. Notes: Any key not appearing in the
 *     first configuration string is discarded from the final result, because we'll never search for
 *     it. Nested structures aren't parsed. For example, imagine a configuration string contains
 *     "key=(k2=v2,k3=v3)", and a subsequent string has "key=(k4=v4)", the result will be
 *     "key=(k4=v4)", as we search for and use the final value of "key", regardless of field overlap
 *     or missing fields in the nested value.
 */
int
__wt_config_collapse(WT_SESSION_IMPL *session, const char **cfg, char **config_ret)
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM k, v;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    *config_ret = NULL;

    WT_RET(__wt_scr_alloc(session, 1024, &tmp));

    __wt_config_init(session, &cparser, cfg[0]);
    while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
        if (k.type != WT_CONFIG_ITEM_STRING && k.type != WT_CONFIG_ITEM_ID)
            WT_ERR_MSG(session, EINVAL, "Invalid configuration key found: '%s'", k.str);
        WT_ERR(__wt_config_get(session, cfg, &k, &v));
        /* Include the quotes around string keys/values. */
        if (k.type == WT_CONFIG_ITEM_STRING)
            WT_CONFIG_PRESERVE_QUOTES(session, &k);
        if (v.type == WT_CONFIG_ITEM_STRING)
            WT_CONFIG_PRESERVE_QUOTES(session, &v);
        WT_ERR(__wt_buf_catfmt(session, tmp, "%.*s=%.*s,", (int)k.len, k.str, (int)v.len, v.str));
    }

    /* We loop until error, and the expected error is WT_NOTFOUND. */
    if (ret != WT_NOTFOUND)
        goto err;

    /*
     * If the caller passes us no valid configuration strings, we get here with no bytes to copy --
     * that's OK, the underlying string copy can handle empty strings.
     *
     * Strip any trailing comma.
     */
    if (tmp->size != 0)
        --tmp->size;
    ret = __wt_strndup(session, tmp->data, tmp->size, config_ret);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * We need a character that can't appear in a key as a separator.
 */
#undef SEP /* separator key, character */
#define SEP "["
#undef SEPC
#define SEPC '['

/*
 * Individual configuration entries, including a generation number used to make the qsort stable.
 */
typedef struct {
    char *k, *v; /* key, value */
    size_t gen;  /* generation */
    bool strip;  /* remove the value */
} WT_CONFIG_MERGE_ENTRY;

/*
 * The array of configuration entries.
 */
typedef struct {
    size_t entries_allocated; /* allocated */
    size_t entries_next;      /* next slot */

    WT_CONFIG_MERGE_ENTRY *entries; /* array of entries */
} WT_CONFIG_MERGE;

/*
 * __config_merge_scan --
 *     Walk a configuration string, inserting entries into the merged array.
 */
static int
__config_merge_scan(
  WT_SESSION_IMPL *session, const char *key, const char *value, bool strip, WT_CONFIG_MERGE *cp)
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM k, v;
    WT_DECL_ITEM(kb);
    WT_DECL_ITEM(vb);
    WT_DECL_RET;
    size_t len;

    WT_ERR(__wt_scr_alloc(session, 1024, &kb));
    WT_ERR(__wt_scr_alloc(session, 1024, &vb));

    __wt_config_init(session, &cparser, value);
    while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
        if (k.type != WT_CONFIG_ITEM_STRING && k.type != WT_CONFIG_ITEM_ID)
            WT_ERR_MSG(session, EINVAL, "Invalid configuration key found: '%s'", k.str);

        /* Include the quotes around string keys/values. */
        if (k.type == WT_CONFIG_ITEM_STRING)
            WT_CONFIG_PRESERVE_QUOTES(session, &k);
        if (v.type == WT_CONFIG_ITEM_STRING)
            WT_CONFIG_PRESERVE_QUOTES(session, &v);

        /*
         * !!!
         * We're using a JSON quote character to separate the names we
         * create for nested structures. That's not completely safe as
         * it's possible to quote characters in JSON such that a quote
         * character appears as a literal character in a key name. In
         * a few cases, applications can create their own key namespace
         * (for example, shared library extension names), and therefore
         * it's possible for an application to confuse us. Error if we
         * we ever see a key with a magic character.
         */
        for (len = 0; len < k.len; ++len)
            if (k.str[len] == SEPC)
                WT_ERR_MSG(session, EINVAL, "key %.*s contains a '%c' separator character",
                  (int)k.len, (char *)k.str, SEPC);

        /* Build the key/value strings. */
        WT_ERR(__wt_buf_fmt(session, kb, "%s%s%.*s", key == NULL ? "" : key, key == NULL ? "" : SEP,
          (int)k.len, k.str));
        WT_ERR(__wt_buf_fmt(session, vb, "%.*s", (int)v.len, v.str));

        /*
         * If the value is a structure, recursively parse it.
         *
         * !!!
         * Don't merge unless the structure has field names. WiredTiger
         * stores checkpoint LSNs in the metadata file using nested
         * structures without field names: "checkpoint_lsn=(1,0)", not
         * "checkpoint_lsn=(file=1,offset=0)". The value type is still
         * WT_CONFIG_ITEM_STRUCT, so we check for a field name in the
         * value.
         */
        if (v.type == WT_CONFIG_ITEM_STRUCT && strchr(vb->data, '=') != NULL) {
            WT_ERR(__config_merge_scan(session, kb->data, vb->data, strip, cp));
            continue;
        }

        /* Insert the value into the array. */
        WT_ERR(
          __wt_realloc_def(session, &cp->entries_allocated, cp->entries_next + 1, &cp->entries));
        WT_ERR(__wt_strndup(session, kb->data, kb->size, &cp->entries[cp->entries_next].k));
        WT_ERR(__wt_strndup(session, vb->data, vb->size, &cp->entries[cp->entries_next].v));
        cp->entries[cp->entries_next].gen = cp->entries_next;
        cp->entries[cp->entries_next].strip = strip;
        ++cp->entries_next;
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_scr_free(session, &kb);
    __wt_scr_free(session, &vb);
    return (ret);
}

/*
 * __strip_comma --
 *     Strip a trailing comma.
 */
static void
__strip_comma(WT_ITEM *buf)
{
    if (buf->size != 0 && ((char *)buf->data)[buf->size - 1] == ',')
        --buf->size;
}

/*
 * __config_merge_format_next --
 *     Walk the array, building entries.
 */
static int
__config_merge_format_next(WT_SESSION_IMPL *session, const char *prefix, size_t plen, size_t *enp,
  WT_CONFIG_MERGE *cp, WT_ITEM *build)
{
    WT_CONFIG_MERGE_ENTRY *ep;
    size_t len1, len2, next, saved_len;
    const char *p;

    for (; *enp < cp->entries_next; ++*enp) {
        ep = &cp->entries[*enp];
        len1 = strlen(ep->k);

        /*
         * The entries are in sorted order, take the last entry for any key.
         */
        if (*enp < (cp->entries_next - 1)) {
            len2 = strlen((ep + 1)->k);

            /* Choose the last of identical keys. */
            if (len1 == len2 && memcmp(ep->k, (ep + 1)->k, len1) == 0)
                continue;

            /*
             * The test is complicated by matching empty entries "foo=" against nested structures
             * "foo,bar=", where the latter is a replacement for the former.
             */
            if (len2 > len1 && (ep + 1)->k[len1] == SEPC && memcmp(ep->k, (ep + 1)->k, len1) == 0)
                continue;
        }

        /*
         * If we're skipping a prefix and this entry doesn't match it, back off one entry and pop up
         * a level.
         */
        if (plen != 0 && (plen > len1 || memcmp(ep->k, prefix, plen) != 0)) {
            --*enp;
            break;
        }

        /*
         * If the entry introduces a new level, recurse through that new level.
         */
        if ((p = strchr(ep->k + plen, SEPC)) != NULL) {
            /* Save the start location of the new level. */
            saved_len = build->size;

            next = WT_PTRDIFF(p, ep->k);
            WT_RET(__wt_buf_catfmt(session, build, "%.*s=(", (int)(next - plen), ep->k + plen));
            WT_RET(__config_merge_format_next(session, ep->k, next + 1, enp, cp, build));
            __strip_comma(build);
            WT_RET(__wt_buf_catfmt(session, build, "),"));

            /*
             * It's possible the level contained nothing, check and discard empty levels.
             */
            p = build->data;
            if (p[build->size - 3] == '(')
                build->size = saved_len;

            continue;
        }

        /* Discard flagged entries. */
        if (ep->strip)
            continue;

        /* Append the entry to the buffer. */
        WT_RET(__wt_buf_catfmt(session, build, "%s=%s,", ep->k + plen, ep->v));
    }

    return (0);
}

/*
 * __config_merge_format --
 *     Take the sorted array of entries, and format them into allocated memory.
 */
static int
__config_merge_format(WT_SESSION_IMPL *session, WT_CONFIG_MERGE *cp, const char **config_ret)
{
    WT_DECL_ITEM(build);
    WT_DECL_RET;
    size_t entries;

    WT_RET(__wt_scr_alloc(session, 4 * 1024, &build));

    entries = 0;
    WT_ERR(__config_merge_format_next(session, "", 0, &entries, cp, build));

    __strip_comma(build);

    ret = __wt_strndup(session, build->data, build->size, config_ret);

err:
    __wt_scr_free(session, &build);
    return (ret);
}

/*
 * __config_merge_cmp --
 *     Qsort function: sort the config merge array.
 */
static int WT_CDECL
__config_merge_cmp(const void *a, const void *b)
{
    WT_CONFIG_MERGE_ENTRY *ae, *be;
    int cmp;

    ae = (WT_CONFIG_MERGE_ENTRY *)a;
    be = (WT_CONFIG_MERGE_ENTRY *)b;

    if ((cmp = strcmp(ae->k, be->k)) != 0)
        return (cmp);
    return (ae->gen > be->gen ? 1 : -1);
}

/*
 * __wt_config_tiered_strip --
 *     Strip any configuration options that should not be persisted in the metadata from the
 *     configuration string.
 */
int
__wt_config_tiered_strip(WT_SESSION_IMPL *session, const char **cfg, const char **config_ret)
{
    const char *strip;

    strip = "tiered_storage=(shared=),";
    return (__wt_config_merge(session, cfg, strip, config_ret));
}

/*
 * __wt_config_merge --
 *     Merge a set of configuration strings into newly allocated memory, optionally discarding
 *     configuration items. This function takes a NULL-terminated list of configuration strings
 *     (where the values are in order from least to most preferred), and merges them into newly
 *     allocated memory. The algorithm is to walk the configuration strings and build a table of
 *     each key/value pair. The pairs are sorted based on the name and the configuration string in
 *     which they were found, and a final configuration string is built from the result.
 *     Additionally, a configuration string can be specified and those configuration values are
 *     removed from the final string. Note: Nested structures are parsed and merged. For example, if
 *     configuration strings "key=(k1=v1,k2=v2)" and "key=(k1=v2)" appear, the result will be
 *     "key=(k1=v2,k2=v2)" because the nested values are merged.
 */
int
__wt_config_merge(WT_SESSION_IMPL *session, const char **cfg, const char *cfg_strip,
  const char **config_ret) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_CONFIG_MERGE merge;
    WT_DECL_RET;
    size_t i;

    /* Start out with a reasonable number of entries. */
    WT_CLEAR(merge);

    WT_RET(__wt_realloc_def(session, &merge.entries_allocated, 100, &merge.entries));

    /*
     * Scan the configuration strings, entering them into the array. The list of configuration
     * values to be removed must be scanned last so their generation numbers are the highest.
     */
    for (; *cfg != NULL; ++cfg)
        WT_ERR(__config_merge_scan(session, NULL, *cfg, false, &merge));
    if (cfg_strip != NULL)
        WT_ERR(__config_merge_scan(session, NULL, cfg_strip, true, &merge));

    /*
     * Sort the array by key and, in the case of identical keys, by generation.
     */
    __wt_qsort(
      merge.entries, merge.entries_next, sizeof(WT_CONFIG_MERGE_ENTRY), __config_merge_cmp);

    /* Convert the array of entries into a string. */
    ret = __config_merge_format(session, &merge, config_ret);

err:
    for (i = 0; i < merge.entries_next; ++i) {
        __wt_free(session, merge.entries[i].k);
        __wt_free(session, merge.entries[i].v);
    }
    __wt_free(session, merge.entries);
    return (ret);
}
