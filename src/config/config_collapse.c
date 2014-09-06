/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * We need a character that can't appear in a key as a separator.
 *
 * XXX
 * I'm not using '.' although that seems like the natural one to use because
 * default checkpoints are named "WiredTiger.#" where dot is part of the key.
 * I think it's wrong, we should not have used a dot in that name, but that's
 * a format change.
 */
#undef	SEP					/* separator key, character */
#define	SEP	","
#undef	SEPC
#define	SEPC	','

/*
 * Individual configuration entries, including a generation number used to make
 * the qsort stable.
 */
typedef struct {
	char  *k, *v;				/* key, value */
	size_t gen;				/* generation */
} WT_COLLAPSE_ENTRY;

/*
 * The array of configuration entries.
 */
typedef struct {
	size_t entries_allocated;		/* allocated */
	size_t entries_next;			/* next slot */

	int nested_replace;			/* replace nested values */

	WT_COLLAPSE_ENTRY *entries;		/* array of entries */
} WT_COLLAPSE;

/*
 * __collapse_scan --
 *	Walk a configuration string, inserting entries into the collapse array.
 */
static int
__collapse_scan(WT_SESSION_IMPL *session,
    const char *key, const char *value, WT_COLLAPSE *cp)
{
	WT_CONFIG cparser;
	WT_CONFIG_ITEM k, v;
	WT_DECL_ITEM(kb);
	WT_DECL_ITEM(vb);
	WT_DECL_RET;

	WT_ERR(__wt_scr_alloc(session, 0, &kb));
	WT_ERR(__wt_scr_alloc(session, 0, &vb));

	WT_ERR(__wt_config_init(session, &cparser, value));
	while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
		if (k.type != WT_CONFIG_ITEM_STRING &&
		    k.type != WT_CONFIG_ITEM_ID)
			WT_ERR_MSG(session, EINVAL,
			    "Invalid configuration key found: '%s'\n", k.str);

		/* Include the quotes around string keys/values. */
		if (k.type == WT_CONFIG_ITEM_STRING) {
			--k.str;
			k.len += 2;
		}
		if (v.type == WT_CONFIG_ITEM_STRING) {
			--v.str;
			v.len += 2;
		}

		/* Build the key/value strings. */
		WT_ERR(__wt_buf_fmt(session,
		    kb, "%s%s%.*s",
		    key == NULL ? "" : key,
		    key == NULL ? "" : SEP,
		    (int)k.len, k.str));
		WT_ERR(__wt_buf_fmt(session,
		    vb, "%.*s", (int)v.len, v.str));

		/*
		 * If the value is a structure, recursively parse it.
		 *
		 * XXX
		 * Problem #1: we store "checkpoint_lsn=(1,0)" in the metadata
		 * file, where the key is type WT_CONFIG_ITEM_ID, the value is
		 * type WT_CONFIG_ITEM_STRUCT. Other nested structures have
		 * field names, should this have been "(file=1,offset=0)"?
		 *
		 * Problem #2: the configuration collapse functions are used by
		 * checkpoint to replace the previous entry in its entirety,
		 * that is, the work we're doing to integrate nested changes
		 * into previous values breaks it.
		 *
		 * We're currently turning off merging nested structures in most
		 * places (including the checkpoint code).
		 */
		if (!cp->nested_replace &&
		    v.type == WT_CONFIG_ITEM_STRUCT &&
		    strchr(vb->data, '=') != NULL) {
			WT_ERR(
			    __collapse_scan(session, kb->data, vb->data, cp));
			continue;
		}

		/* Insert the value into the array. */
		WT_ERR(__wt_realloc_def(session,
		    &cp->entries_allocated,
		    cp->entries_next + 1, &cp->entries));
		WT_ERR(__wt_strndup(session,
		    kb->data, kb->size, &cp->entries[cp->entries_next].k));
		WT_ERR(__wt_strndup(session,
		    vb->data, vb->size, &cp->entries[cp->entries_next].v));
		cp->entries[cp->entries_next].gen = cp->entries_next;
		++cp->entries_next;
	}

err:	__wt_scr_free(&kb);
	__wt_scr_free(&vb);
	return (0);
}

/*
 * __strip_comma --
 *	Strip a trailing comma.
 */
static inline void
__strip_comma(WT_ITEM *buf)
{
	if (buf->size != 0 && ((char *)buf->data)[buf->size - 1] == ',')
		--buf->size;
}

/*
 * __collapse_format_next --
 *	Walk the array, building entries.
 */
static int
__collapse_format_next(WT_SESSION_IMPL *session, const char *prefix,
    size_t plen, size_t *enp, WT_COLLAPSE *cp, WT_ITEM *build)
{
	WT_COLLAPSE_ENTRY *ep;
	size_t len1, len2, next;
	char *p;

	for (; *enp < cp->entries_next; ++*enp) {
		ep = &cp->entries[*enp];

		/*
		 * The entries are in sorted order, take the last entry for any
		 * key.
		 */
		if (*enp < (cp->entries_next - 1)) {
			len1 = strlen(ep->k);
			len2 = strlen((ep + 1)->k);

			/* Choose the last of identical keys. */
			if (len1 == len2 &&
			    memcmp(ep->k, (ep + 1)->k, len1) == 0)
				continue;

			/*
			 * The test is complicated by matching empty entries
			 * "foo=" against nested structures "foo,bar=", where
			 * the latter is a replacement for the former.
			 */
			if (len2 > len1 &&
			    (ep + 1)->k[len1] == SEPC &&
			    memcmp(ep->k, (ep + 1)->k, len1) == 0)
				continue;
		}

		/*
		 * If we're skipping a prefix and this entry doesn't match it,
		 * back off one entry and pop up a level.
		 */
		if (plen != 0 && memcmp(ep->k, prefix, plen) != 0) {
			--*enp;
			break;
		}

		/*
		 * If the entry introduces a new level, recurse through that
		 * new level.
		 */
		if ((p = strchr(ep->k + plen, SEPC)) != NULL) {
			next = WT_PTRDIFF(p, ep->k);
			WT_RET(__wt_buf_catfmt(session,
			    build, "%.*s=(", (int)(next - plen), ep->k + plen));
			WT_RET(__collapse_format_next(
			    session, ep->k, next + 1, enp, cp, build));
			__strip_comma(build);
			WT_RET(__wt_buf_catfmt(session, build, "),"));
			continue;
		}

		/* Append the entry to the buffer. */
		WT_RET(__wt_buf_catfmt(
		    session, build, "%s=%s,", ep->k + plen, ep->v));
	}

	return (0);
}

/*
 * __collapse_format --
 *	Take the sorted array of entries, and format them into allocated memory.
 */
static int
__collapse_format(
    WT_SESSION_IMPL *session, WT_COLLAPSE *cp, const char **config_ret)
{
	WT_DECL_ITEM(build);
	WT_DECL_RET;
	size_t entries;

	WT_RET(__wt_scr_alloc(session, 4 * 1024, &build));

	entries = 0;
	WT_ERR(__collapse_format_next(session, "", 0, &entries, cp, build));

	__strip_comma(build);

	ret = __wt_strndup(session, build->data, build->size, config_ret);

err:	__wt_scr_free(&build);
	return (ret);
}

/*
 * __collapse_cmp --
 *	Qsort function: sort the collapse array.
 */
static int
__collapse_cmp(const void *a, const void *b)
{
	WT_COLLAPSE_ENTRY *ae, *be;
	int cmp;

	ae = (WT_COLLAPSE_ENTRY *)a;
	be = (WT_COLLAPSE_ENTRY *)b;

	if ((cmp = strcmp(ae->k, be->k)) != 0)
		return (cmp);
	return (ae->gen > be->gen ? 1 : -1);
}

/*
 * __wt_config_collapse --
 *	Given a NULL-terminated list of configuration strings, in reverse order
 * of preference (the first set of strings are the least preferred), collapse
 * them into allocated memory.
 */
int
__wt_config_collapse(WT_SESSION_IMPL *session,
    const char **cfg, const char **config_ret, int nested_replace)
{
	WT_COLLAPSE collapse;
	WT_DECL_RET;
	size_t i;

	/* Start out with a reasonable number of entries. */
	WT_CLEAR(collapse);
	collapse.nested_replace = nested_replace;

	WT_RET(__wt_realloc_def(
	    session, &collapse.entries_allocated, 100, &collapse.entries));

	/* Scan the configuration strings, entering them into the array. */
	for (; *cfg != NULL; ++cfg)
		WT_ERR(__collapse_scan(session, NULL, *cfg, &collapse));

	/*
	 * Sort the array by key and, in the case of identical keys, by
	 * generation.
	 */
	qsort(collapse.entries,
	    collapse.entries_next, sizeof(WT_COLLAPSE_ENTRY), __collapse_cmp);

	/* Convert the array of entries into a string. */
	ret = __collapse_format(session, &collapse, config_ret);

err:	for (i = 0; i < collapse.entries_next; ++i) {
		__wt_free(session, collapse.entries[i].k);
		__wt_free(session, collapse.entries[i].v);
	}
	__wt_free(session, collapse.entries);
	return (ret);
}
