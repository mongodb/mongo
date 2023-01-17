/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __curjoin_entries_in_range(
  WT_SESSION_IMPL *, WT_CURSOR_JOIN *, WT_ITEM *, WT_CURSOR_JOIN_ITER *);
static int __curjoin_entry_in_range(
  WT_SESSION_IMPL *, WT_CURSOR_JOIN_ENTRY *, WT_ITEM *, WT_CURSOR_JOIN_ITER *);
static int __curjoin_entry_member(
  WT_SESSION_IMPL *, WT_CURSOR_JOIN_ENTRY *, WT_ITEM *, WT_CURSOR_JOIN_ITER *);
static int __curjoin_insert_endpoint(
  WT_SESSION_IMPL *, WT_CURSOR_JOIN_ENTRY *, u_int, WT_CURSOR_JOIN_ENDPOINT **);
static int __curjoin_iter_close(WT_CURSOR_JOIN_ITER *);
static int __curjoin_iter_close_all(WT_CURSOR_JOIN_ITER *);
static bool __curjoin_iter_ready(WT_CURSOR_JOIN_ITER *);
static int __curjoin_iter_set_entry(WT_CURSOR_JOIN_ITER *, u_int);
static int __curjoin_pack_recno(WT_SESSION_IMPL *, uint64_t, uint8_t *, size_t, WT_ITEM *);
static int __curjoin_split_key(
  WT_SESSION_IMPL *, WT_CURSOR_JOIN *, WT_ITEM *, WT_CURSOR *, WT_CURSOR *, const char *, bool);

#define WT_CURJOIN_ITER_CONSUMED(iter) ((iter)->entry_pos >= (iter)->entry_count)

/*
 * __wt_curjoin_joined --
 *     Produce an error that this cursor is being used in a join call.
 */
int
__wt_curjoin_joined(WT_CURSOR *cursor) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cursor);

    WT_RET_MSG(session, ENOTSUP, "cursor is being used in a join");
}

/*
 * __curjoin_iter_init --
 *     Initialize an iteration for the index managed by a join entry.
 */
static int
__curjoin_iter_init(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin, WT_CURSOR_JOIN_ITER **iterp)
{
    WT_CURSOR_JOIN_ITER *iter;

    *iterp = NULL;

    WT_RET(__wt_calloc_one(session, iterp));
    iter = *iterp;
    iter->cjoin = cjoin;
    iter->session = session;
    cjoin->iter = iter;
    WT_RET(__curjoin_iter_set_entry(iter, 0));
    return (0);
}

/*
 * __curjoin_iter_close --
 *     Close the iteration, release resources.
 */
static int
__curjoin_iter_close(WT_CURSOR_JOIN_ITER *iter)
{
    WT_DECL_RET;

    if (iter->cursor != NULL)
        WT_TRET(iter->cursor->close(iter->cursor));
    __wt_free(iter->session, iter);
    return (ret);
}

/*
 * __curjoin_iter_close_all --
 *     Free the iterator and all of its children recursively.
 */
static int
__curjoin_iter_close_all(WT_CURSOR_JOIN_ITER *iter)
{
    WT_CURSOR_JOIN *parent;
    WT_DECL_RET;

    if (iter->child)
        WT_TRET(__curjoin_iter_close_all(iter->child));
    iter->child = NULL;
    WT_ASSERT(
      iter->session, iter->cjoin->parent == NULL || iter->cjoin->parent->iter->child == iter);
    if ((parent = iter->cjoin->parent) != NULL)
        parent->iter->child = NULL;
    iter->cjoin->iter = NULL;
    WT_TRET(__curjoin_iter_close(iter));
    return (ret);
}

/*
 * __curjoin_iter_reset --
 *     Reset an iteration to the starting point.
 */
static int
__curjoin_iter_reset(WT_CURSOR_JOIN_ITER *iter)
{
    if (iter->child != NULL)
        WT_RET(__curjoin_iter_close_all(iter->child));
    WT_RET(__curjoin_iter_set_entry(iter, 0));
    iter->positioned = false;
    return (0);
}

/*
 * __curjoin_iter_ready --
 *     Check the positioned flag for all nested iterators.
 */
static bool
__curjoin_iter_ready(WT_CURSOR_JOIN_ITER *iter)
{
    while (iter != NULL) {
        if (!iter->positioned)
            return (false);
        iter = iter->child;
    }
    return (true);
}

/*
 * __curjoin_iter_set_entry --
 *     Set the current entry for an iterator.
 */
static int
__curjoin_iter_set_entry(WT_CURSOR_JOIN_ITER *iter, u_int entry_pos)
{
    WT_CURSOR *c, *to_dup;
    WT_CURSOR_JOIN *cjoin, *topjoin;
    WT_CURSOR_JOIN_ENTRY *entry;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    size_t size;
    char *uri;
    const char **config;
    const char *def_cfg[] = {WT_CONFIG_BASE(iter->session, WT_SESSION_open_cursor), NULL};
    const char *raw_cfg[] = {WT_CONFIG_BASE(iter->session, WT_SESSION_open_cursor), "raw", NULL};

    session = iter->session;
    cjoin = iter->cjoin;
    uri = NULL;
    entry = iter->entry = &cjoin->entries[entry_pos];
    iter->positioned = false;
    iter->entry_pos = entry_pos;
    iter->end_pos = 0;

    iter->is_equal =
      (entry->ends_next == 1 && WT_CURJOIN_END_RANGE(&entry->ends[0]) == WT_CURJOIN_END_EQ);
    iter->end_skip =
      (entry->ends_next > 0 && WT_CURJOIN_END_RANGE(&entry->ends[0]) == WT_CURJOIN_END_GE) ? 1 : 0;

    iter->end_count = WT_MIN(1, entry->ends_next);
    if (F_ISSET(cjoin, WT_CURJOIN_DISJUNCTION)) {
        iter->entry_count = cjoin->entries_next;
        if (iter->is_equal)
            iter->end_count = entry->ends_next;
    } else
        iter->entry_count = 1;
    WT_ASSERT(iter->session, iter->entry_pos < iter->entry_count);

    entry->stats.iterated = 0;

    if (entry->subjoin == NULL) {
        for (topjoin = iter->cjoin; topjoin->parent != NULL; topjoin = topjoin->parent)
            ;
        to_dup = entry->ends[0].cursor;

        if (F_ISSET((WT_CURSOR *)topjoin, WT_CURSTD_RAW))
            config = &raw_cfg[0];
        else
            config = &def_cfg[0];

        size = strlen(to_dup->internal_uri) + 3;
        WT_ERR(__wt_calloc(session, size, 1, &uri));
        WT_ERR(__wt_snprintf(uri, size, "%s()", to_dup->internal_uri));
        if ((c = iter->cursor) == NULL || strcmp(c->uri, uri) != 0) {
            iter->cursor = NULL;
            if (c != NULL)
                WT_ERR(c->close(c));
            WT_ERR(__wt_open_cursor(session, uri, (WT_CURSOR *)topjoin, config, &iter->cursor));
        }
        WT_ERR(__wt_cursor_dup_position(to_dup, iter->cursor));
    } else if (iter->cursor != NULL) {
        WT_ERR(iter->cursor->close(iter->cursor));
        iter->cursor = NULL;
    }

err:
    __wt_free(session, uri);
    return (ret);
}

/*
 * __curjoin_iter_bump --
 *     Called to advance the iterator to the next endpoint, which may in turn advance to the next
 *     entry. We cannot skip a call to this at the end of an iteration because we'll need to advance
 *     the position to the end.
 */
static int
__curjoin_iter_bump(WT_CURSOR_JOIN_ITER *iter)
{
    WT_CURSOR_JOIN_ENTRY *entry;
    WT_SESSION_IMPL *session;

    session = iter->session;
    iter->positioned = false;
    entry = iter->entry;
    if (entry->subjoin == NULL && iter->is_equal && ++iter->end_pos < iter->end_count) {
        WT_RET(__wt_cursor_dup_position(entry->ends[iter->end_pos].cursor, iter->cursor));
        return (0);
    }
    iter->end_pos = iter->end_count = iter->end_skip = 0;
    if (entry->subjoin != NULL && entry->subjoin->iter != NULL)
        WT_RET(__curjoin_iter_close_all(entry->subjoin->iter));

    if (++iter->entry_pos >= iter->entry_count) {
        iter->entry = NULL;
        return (0);
    }
    iter->entry = ++entry;
    if (entry->subjoin != NULL) {
        WT_RET(__curjoin_iter_init(session, entry->subjoin, &iter->child));
        return (0);
    }
    WT_RET(__curjoin_iter_set_entry(iter, iter->entry_pos));
    return (0);
}

/*
 * __curjoin_iter_next --
 *     Get the next item in an iteration.
 */
static int
__curjoin_iter_next(WT_CURSOR_JOIN_ITER *iter, WT_CURSOR *cursor)
{
    WT_CURSOR_JOIN_ENTRY *entry;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = iter->session;

    if (WT_CURJOIN_ITER_CONSUMED(iter))
        return (WT_NOTFOUND);
again:
    entry = iter->entry;
    if (entry->subjoin != NULL) {
        if (iter->child == NULL)
            WT_RET(__curjoin_iter_init(session, entry->subjoin, &iter->child));
        ret = __curjoin_iter_next(iter->child, cursor);
        if (ret == 0) {
            /* The child did the work, we're done. */
            iter->curkey = &cursor->key;
            iter->positioned = true;
            return (ret);
        }
        if (ret == WT_NOTFOUND) {
            WT_RET(__curjoin_iter_close_all(iter->child));
            entry->subjoin->iter = NULL;
            iter->child = NULL;
            WT_RET(__curjoin_iter_bump(iter));
            ret = 0;
        }
    } else if (iter->positioned) {
        ret = iter->cursor->next(iter->cursor);
        if (ret == WT_NOTFOUND) {
            WT_RET(__curjoin_iter_bump(iter));
            ret = 0;
        } else
            WT_RET(ret);
    } else
        iter->positioned = true;

    if (WT_CURJOIN_ITER_CONSUMED(iter))
        return (WT_NOTFOUND);

    if (!__curjoin_iter_ready(iter))
        goto again;

    WT_RET(ret);

    /*
     * Set our key to the primary key, we'll also need this to check membership.
     */
    WT_RET(__curjoin_split_key(iter->session, iter->cjoin, &iter->idxkey, cursor, iter->cursor,
      iter->entry->repack_format, iter->entry->index != NULL));
    iter->curkey = &cursor->key;
    iter->entry->stats.iterated++;
    return (0);
}

/*
 * __curjoin_close --
 *     WT_CURSOR::close for join cursors.
 */
static int
__curjoin_close(WT_CURSOR *cursor)
{
    WT_CURSOR_JOIN *cjoin;
    WT_CURSOR_JOIN_ENDPOINT *end;
    WT_CURSOR_JOIN_ENTRY *entry;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;

    cjoin = (WT_CURSOR_JOIN *)cursor;
    JOINABLE_CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);
err:

    WT_TRET(__wt_schema_release_table(session, &cjoin->table));

    /* This is owned by the table */
    cursor->key_format = NULL;
    if (cjoin->projection != NULL) {
        __wt_free(session, cjoin->projection);
        __wt_free(session, cursor->value_format);
    }

    for (entry = cjoin->entries, i = 0; i < cjoin->entries_next; entry++, i++) {
        if (entry->subjoin != NULL) {
            F_CLR(&entry->subjoin->iface, WT_CURSTD_JOINED);
            entry->subjoin->parent = NULL;
        }
        if (entry->main != NULL)
            WT_TRET(entry->main->close(entry->main));
        if (F_ISSET(entry, WT_CURJOIN_ENTRY_OWN_BLOOM))
            WT_TRET(__wt_bloom_close(entry->bloom));
        for (end = &entry->ends[0]; end < &entry->ends[entry->ends_next]; end++) {
            F_CLR(end->cursor, WT_CURSTD_JOINED);
            if (F_ISSET(end, WT_CURJOIN_END_OWN_CURSOR))
                WT_TRET(end->cursor->close(end->cursor));
        }
        __wt_free(session, entry->ends);
        __wt_free(session, entry->repack_format);
    }

    if (cjoin->iter != NULL)
        WT_TRET(__curjoin_iter_close_all(cjoin->iter));
    if (cjoin->main != NULL)
        WT_TRET(cjoin->main->close(cjoin->main));

    __wt_free(session, cjoin->entries);
    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __curjoin_endpoint_init_key --
 *     Set the key in the reference endpoint.
 */
static int
__curjoin_endpoint_init_key(
  WT_SESSION_IMPL *session, WT_CURSOR_JOIN_ENTRY *entry, WT_CURSOR_JOIN_ENDPOINT *endpoint)
{
    WT_CURSOR *cursor;
    WT_CURSOR_INDEX *cindex;
    WT_ITEM *k;
    uint64_t r;

    if ((cursor = endpoint->cursor) != NULL) {
        if (entry->index != NULL) {
            /* Extract and save the index's logical key. */
            cindex = (WT_CURSOR_INDEX *)endpoint->cursor;
            WT_RET(__wt_struct_repack(session, cindex->child->key_format,
              (entry->repack_format != NULL ? entry->repack_format : cindex->iface.key_format),
              &cindex->child->key, &endpoint->key));
        } else {
            k = &((WT_CURSOR_TABLE *)cursor)->cg_cursors[0]->key;
            if (WT_CURSOR_RECNO(cursor)) {
                r = *(uint64_t *)k->data;
                WT_RET(__curjoin_pack_recno(
                  session, r, endpoint->recno_buf, sizeof(endpoint->recno_buf), &endpoint->key));
            } else
                endpoint->key = *k;
        }
    }
    return (0);
}

/*
 * __curjoin_entries_in_range --
 *     Check if a key is in the range specified by the remaining entries, returning WT_NOTFOUND if
 *     not.
 */
static int
__curjoin_entries_in_range(
  WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin, WT_ITEM *curkey, WT_CURSOR_JOIN_ITER *iterarg)
{
    WT_CURSOR_JOIN_ENTRY *entry;
    WT_CURSOR_JOIN_ITER *iter;
    WT_DECL_RET;
    u_int pos;
    int fastret, slowret;

    iter = iterarg;
    if (F_ISSET(cjoin, WT_CURJOIN_DISJUNCTION)) {
        fastret = 0;
        slowret = WT_NOTFOUND;
    } else {
        fastret = WT_NOTFOUND;
        slowret = 0;
    }
    pos = iter == NULL ? 0 : iter->entry_pos;
    for (entry = &cjoin->entries[pos]; pos < cjoin->entries_next; entry++, pos++) {
        ret = __curjoin_entry_member(session, entry, curkey, iter);
        if (ret == fastret)
            return (fastret);
        if (ret != slowret)
            break;
        iter = NULL;
    }

    return (ret == 0 ? slowret : ret);
}

/*
 * __curjoin_entry_in_range --
 *     Check if a key is in the range specified by the entry, returning WT_NOTFOUND if not.
 */
static int
__curjoin_entry_in_range(
  WT_SESSION_IMPL *session, WT_CURSOR_JOIN_ENTRY *entry, WT_ITEM *curkey, WT_CURSOR_JOIN_ITER *iter)
{
    WT_COLLATOR *collator;
    WT_CURSOR_JOIN_ENDPOINT *end, *endmax;
    u_int pos;
    int cmp;
    bool disjunction, passed;

    collator = (entry->index != NULL) ? entry->index->collator : NULL;
    endmax = &entry->ends[entry->ends_next];
    disjunction = F_ISSET(entry, WT_CURJOIN_ENTRY_DISJUNCTION);

    /*
     * The iterator may have already satisfied some endpoint conditions. If so and we're a
     * disjunction, we're done. If so and we're a conjunction, we can start past the satisfied
     * conditions.
     */
    if (iter == NULL)
        pos = 0;
    else {
        if (disjunction && iter->end_skip)
            return (0);
        pos = iter->end_pos + iter->end_skip;
    }

    for (end = &entry->ends[pos]; end < endmax; end++) {
        WT_RET(__wt_compare(session, collator, curkey, &end->key, &cmp));
        switch (WT_CURJOIN_END_RANGE(end)) {
        case WT_CURJOIN_END_EQ:
            passed = (cmp == 0);
            break;

        case WT_CURJOIN_END_GT | WT_CURJOIN_END_EQ:
            passed = (cmp >= 0);
            WT_ASSERT(session, iter == NULL);
            break;

        case WT_CURJOIN_END_GT:
            passed = (cmp > 0);
            if (passed && iter != NULL && pos == 0)
                iter->end_skip = 1;
            break;

        case WT_CURJOIN_END_LT | WT_CURJOIN_END_EQ:
            passed = (cmp <= 0);
            break;

        case WT_CURJOIN_END_LT:
            passed = (cmp < 0);
            break;

        default:
            return (__wt_illegal_value(session, WT_CURJOIN_END_RANGE(end)));
        }

        if (!passed) {
            if (iter != NULL && (iter->is_equal || F_ISSET(end, WT_CURJOIN_END_LT))) {
                /*
                 * Even though this cursor is done, we still need to bump (advance it), to mark the
                 * iteration as complete.
                 */
                WT_RET(__curjoin_iter_bump(iter));
                return (WT_NOTFOUND);
            }
            if (!disjunction)
                return (WT_NOTFOUND);
            iter = NULL;
        } else if (disjunction)
            break;
    }
    if (disjunction && end == endmax)
        return (WT_NOTFOUND);
    return (0);
}

typedef struct {
    WT_CURSOR iface;
    WT_CURSOR_JOIN_ENTRY *entry;
    bool ismember;
} WT_CURJOIN_EXTRACTOR;

/*
 * __curjoin_extract_insert --
 *     Handle a key produced by a custom extractor.
 */
static int
__curjoin_extract_insert(WT_CURSOR *cursor)
{
    WT_CURJOIN_EXTRACTOR *cextract;
    WT_DECL_RET;
    WT_ITEM ikey;
    WT_SESSION_IMPL *session;

    /*
     * This insert method may be called multiple times during a single extraction. If we already
     * have a definitive answer to the membership question, exit early.
     */
    cextract = (WT_CURJOIN_EXTRACTOR *)cursor;
    if (cextract->ismember)
        return (0);

    CURSOR_API_CALL(cursor, session, insert, NULL);

    WT_ITEM_SET(ikey, cursor->key);
    /*
     * We appended a padding byte to the key to avoid rewriting the last column. Strip that away
     * here.
     */
    WT_ASSERT(session, ikey.size > 0);
    --ikey.size;

    ret = __curjoin_entry_in_range(session, cextract->entry, &ikey, NULL);
    if (ret == WT_NOTFOUND)
        ret = 0;
    else if (ret == 0)
        cextract->ismember = true;

err:
    API_END_RET(session, ret);
}

/*
 * __curjoin_entry_member --
 *     Do a membership check for a particular index that was joined, if not a member, returns
 *     WT_NOTFOUND.
 */
static int
__curjoin_entry_member(
  WT_SESSION_IMPL *session, WT_CURSOR_JOIN_ENTRY *entry, WT_ITEM *key, WT_CURSOR_JOIN_ITER *iter)
{
    WT_CURJOIN_EXTRACTOR extract_cursor;
    WT_CURSOR *c;
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value,                          /* get-value */
      __wt_cursor_get_raw_key_value,                  /* get-raw-key-value */
      __wt_cursor_set_key,                            /* set-key */
      __wt_cursor_set_value,                          /* set-value */
      __wt_cursor_compare_notsup,                     /* compare */
      __wt_cursor_equals_notsup,                      /* equals */
      __wt_cursor_notsup,                             /* next */
      __wt_cursor_notsup,                             /* prev */
      __wt_cursor_notsup,                             /* reset */
      __wt_cursor_notsup,                             /* search */
      __wt_cursor_search_near_notsup,                 /* search-near */
      __curjoin_extract_insert,                       /* insert */
      __wt_cursor_modify_notsup,                      /* modify */
      __wt_cursor_notsup,                             /* update */
      __wt_cursor_notsup,                             /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_config_notsup,                      /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __wt_cursor_config_notsup,                      /* bound */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __wt_cursor_notsup);                            /* close */
    WT_DECL_RET;
    WT_INDEX *idx;
    WT_ITEM v;
    bool bloom_found;

    /* We cannot have a bloom filter on a join entry with subordinates. */
    WT_ASSERT(session, entry->bloom == NULL || entry->subjoin == NULL);

    if (entry->subjoin == NULL && iter != NULL &&
      (iter->end_pos + iter->end_skip >= entry->ends_next ||
        (iter->end_skip > 0 && F_ISSET(entry, WT_CURJOIN_ENTRY_DISJUNCTION))))
        return (0); /* no checks to make */

    entry->stats.membership_check++;
    bloom_found = false;

    if (entry->bloom != NULL) {
        /*
         * If the item is not in the Bloom filter, we return immediately, otherwise, we still may
         * need to check the long way, since it may be a false positive.
         *
         * If we don't own the Bloom filter, we must be sharing one in a previous entry. So the
         * shared filter has already been checked and passed, we don't need to check it again. We'll
         * still need to check the long way.
         */
        if (F_ISSET(entry, WT_CURJOIN_ENTRY_OWN_BLOOM))
            WT_ERR(__wt_bloom_inmem_get(entry->bloom, key));
        if (F_ISSET(entry, WT_CURJOIN_ENTRY_FALSE_POSITIVES))
            return (0);
        bloom_found = true;
    }
    if (entry->subjoin != NULL) {
        /*
         * If we have a subordinate join, the membership check is delegated to it.
         */
        WT_ASSERT(session, iter == NULL || entry->subjoin == iter->child->cjoin);
        WT_ERR(__curjoin_entries_in_range(
          session, entry->subjoin, key, iter == NULL ? NULL : iter->child));
        if (iter != NULL && WT_CURJOIN_ITER_CONSUMED(iter->child))
            return (WT_NOTFOUND);
        /* There's nothing more to do for this node. */
        return (0);
    }
    if (entry->index != NULL) {
        /*
         * If this entry is used by the iterator, then we already have the index key, and we won't
         * have to do any extraction either.
         */
        if (iter != NULL && entry == iter->entry)
            WT_ITEM_SET(v, iter->idxkey);
        else {
            memset(&v, 0, sizeof(v)); /* Keep lint quiet. */
            c = entry->main;
            c->set_key(c, key);
            entry->stats.main_access++;
            if ((ret = c->search(c)) == 0)
                ret = c->get_value(c, &v);
            else if (ret == WT_NOTFOUND) {
                __wt_err(session, ret, "main table for join is missing entry");
                ret = WT_ERROR;
            }
            WT_TRET(c->reset(c));
            WT_ERR(ret);
        }
    } else
        WT_ITEM_SET(v, *key);

    if ((idx = entry->index) != NULL && idx->extractor != NULL &&
      (iter == NULL || entry != iter->entry)) {
        WT_CLEAR(extract_cursor);
        extract_cursor.iface = iface;
        extract_cursor.iface.session = &session->iface;
        extract_cursor.iface.key_format = idx->exkey_format;
        extract_cursor.ismember = false;
        extract_cursor.entry = entry;
        WT_ERR(
          idx->extractor->extract(idx->extractor, &session->iface, key, &v, &extract_cursor.iface));
        __wt_buf_free(session, &extract_cursor.iface.key);
        __wt_buf_free(session, &extract_cursor.iface.value);
        if (!extract_cursor.ismember)
            WT_ERR(WT_NOTFOUND);
    } else
        WT_ERR(__curjoin_entry_in_range(session, entry, &v, iter));

    if (0) {
err:
        if (ret == WT_NOTFOUND && bloom_found)
            entry->stats.bloom_false_positive++;
    }
    return (ret);
}

/*
 * __curjoin_get_key --
 *     WT_CURSOR->get_key for join cursors.
 */
static int
__curjoin_get_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR_JOIN *cjoin;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    cjoin = (WT_CURSOR_JOIN *)cursor;

    JOINABLE_CURSOR_API_CALL(cursor, session, get_key, NULL);

    if (!F_ISSET(cjoin, WT_CURJOIN_INITIALIZED) || !cjoin->iter->positioned)
        WT_ERR_MSG(session, EINVAL, "join cursor must be advanced with next()");
    va_start(ap, cursor);
    ret = __wt_cursor_get_keyv(cursor, cursor->flags, ap);
    va_end(ap);

err:
    API_END_RET(session, ret);
}

/*
 * __curjoin_get_value --
 *     WT_CURSOR->get_value for join cursors.
 */
static int
__curjoin_get_value(WT_CURSOR *cursor, ...)
{
    WT_CURSOR_JOIN *cjoin;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    cjoin = (WT_CURSOR_JOIN *)cursor;

    JOINABLE_CURSOR_API_CALL(cursor, session, get_value, NULL);

    if (!F_ISSET(cjoin, WT_CURJOIN_INITIALIZED) || !cjoin->iter->positioned)
        WT_ERR_MSG(session, EINVAL, "join cursor must be advanced with next()");

    va_start(ap, cursor);
    ret = __wt_curtable_get_valuev(cjoin->main, ap);
    va_end(ap);

err:
    API_END_RET(session, ret);
}

/*
 * __curjoin_init_bloom --
 *     Populate Bloom filters
 */
static int
__curjoin_init_bloom(
  WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin, WT_CURSOR_JOIN_ENTRY *entry, WT_BLOOM *bloom)
{
    WT_COLLATOR *collator;
    WT_CURSOR *c;
    WT_CURSOR_JOIN_ENDPOINT *end, *endmax;
    WT_DECL_ITEM(uribuf);
    WT_DECL_RET;
    WT_ITEM curkey, curvalue;
    size_t size;
    u_int skip;
    int cmp;
    const char *raw_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "raw", NULL};
    const char *uri;

    c = NULL;
    skip = 0;

    if (entry->index != NULL)
        /*
         * Open the raw index. We're avoiding any references to the main table, they may be
         * expensive.
         */
        uri = entry->index->source;
    else {
        /*
         * For joins on the main table, we just need the primary key for comparison, we don't need
         * any values.
         */
        size = strlen(cjoin->table->iface.name) + 3;
        WT_ERR(__wt_scr_alloc(session, size, &uribuf));
        WT_ERR(__wt_buf_fmt(session, uribuf, "%s()", cjoin->table->iface.name));
        uri = uribuf->data;
    }
    WT_ERR(__wt_open_cursor(session, uri, &cjoin->iface, raw_cfg, &c));

    /* Initially position the cursor if necessary. */
    endmax = &entry->ends[entry->ends_next];
    if ((end = &entry->ends[0]) < endmax) {
        if (F_ISSET(end, WT_CURJOIN_END_GT) || WT_CURJOIN_END_RANGE(end) == WT_CURJOIN_END_EQ) {
            WT_ERR(__wt_cursor_dup_position(end->cursor, c));
            if (WT_CURJOIN_END_RANGE(end) == WT_CURJOIN_END_GE)
                skip = 1;
        } else if (F_ISSET(end, WT_CURJOIN_END_LT)) {
            if ((ret = c->next(c)) == WT_NOTFOUND)
                goto done;
            WT_ERR(ret);
        } else
            WT_ERR_PANIC(session, EINVAL, "fatal error in join cursor position state");
    }
    collator = (entry->index == NULL) ? NULL : entry->index->collator;
    while (ret == 0) {
        WT_ERR(c->get_key(c, &curkey));
        entry->stats.iterated++;
        if (entry->index != NULL) {
            /*
             * Repack so it's comparable to the reference endpoints.
             */
            WT_ERR(__wt_struct_repack(session, c->key_format,
              (entry->repack_format != NULL ? entry->repack_format : entry->index->idxkey_format),
              &c->key, &curkey));
        }
        for (end = &entry->ends[skip]; end < endmax; end++) {
            WT_ERR(__wt_compare(session, collator, &curkey, &end->key, &cmp));
            if (F_ISSET(entry, WT_CURJOIN_ENTRY_DISJUNCTION)) {
                /* if condition satisfied, insert immediately */
                switch (WT_CURJOIN_END_RANGE(end)) {
                case WT_CURJOIN_END_EQ:
                    if (cmp == 0)
                        goto insert;
                    break;
                case WT_CURJOIN_END_GT:
                    if (cmp > 0) {
                        /* skip this check next time */
                        skip = entry->ends_next;
                        goto insert;
                    }
                    break;
                case WT_CURJOIN_END_GE:
                    if (cmp >= 0)
                        goto insert;
                    break;
                case WT_CURJOIN_END_LT:
                    if (cmp < 0)
                        goto insert;
                    break;
                case WT_CURJOIN_END_LE:
                    if (cmp <= 0)
                        goto insert;
                    break;
                }
            } else if (!F_ISSET(end, WT_CURJOIN_END_LT)) {
                if (cmp < 0 || (cmp == 0 && !F_ISSET(end, WT_CURJOIN_END_EQ)))
                    goto advance;
                if (cmp > 0) {
                    if (F_ISSET(end, WT_CURJOIN_END_GT))
                        skip = 1;
                    else
                        goto done;
                }
            } else {
                if (cmp > 0 || (cmp == 0 && !F_ISSET(end, WT_CURJOIN_END_EQ)))
                    goto done;
            }
        }
        /*
         * Either it's a disjunction that hasn't satisfied any condition, or it's a conjunction that
         * has satisfied all conditions.
         */
        if (F_ISSET(entry, WT_CURJOIN_ENTRY_DISJUNCTION))
            goto advance;
insert:
        if (entry->index != NULL) {
            curvalue.data = (unsigned char *)curkey.data + curkey.size;
            WT_ASSERT(session, c->key.size > curkey.size);
            curvalue.size = c->key.size - curkey.size;
        } else
            WT_ERR(c->get_key(c, &curvalue));
        __wt_bloom_insert(bloom, &curvalue);
        entry->stats.bloom_insert++;
advance:
        if ((ret = c->next(c)) == WT_NOTFOUND)
            break;
    }
done:
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    if (c != NULL)
        WT_TRET(c->close(c));
    __wt_scr_free(session, &uribuf);
    return (ret);
}

/*
 * __curjoin_init_next --
 *     Initialize the cursor join when the next function is first called.
 */
static int
__curjoin_init_next(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin, bool iterable)
{
    WT_BLOOM *bloom;
    WT_CURSOR *origcur;
    WT_CURSOR_JOIN_ENDPOINT *end;
    WT_CURSOR_JOIN_ENTRY *je, *jeend, *je2;
    WT_DECL_RET;
    size_t size;
    uint32_t f, k;
    char *mainbuf;
    const char **config, *proj, *urimain;
    const char *def_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};
    const char *raw_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "raw", NULL};

    mainbuf = NULL;
    if (cjoin->entries_next == 0)
        WT_RET_MSG(session, EINVAL, "join cursor has not yet been joined with any other cursors");

    /* Get a consistent view of our subordinate cursors if appropriate. */
    __wt_txn_cursor_op(session);

    if (F_ISSET((WT_CURSOR *)cjoin, WT_CURSTD_RAW))
        config = &raw_cfg[0];
    else
        config = &def_cfg[0];
    urimain = cjoin->table->iface.name;
    if ((proj = cjoin->projection) != NULL) {
        size = strlen(urimain) + strlen(proj) + 1;
        WT_ERR(__wt_calloc(session, size, 1, &mainbuf));
        WT_ERR(__wt_snprintf(mainbuf, size, "%s%s", urimain, proj));
        urimain = mainbuf;
    }
    WT_ERR(__wt_open_cursor(session, urimain, (WT_CURSOR *)cjoin, config, &cjoin->main));

    jeend = &cjoin->entries[cjoin->entries_next];
    for (je = cjoin->entries; je < jeend; je++) {
        if (je->subjoin != NULL) {
            WT_ERR(__curjoin_init_next(session, je->subjoin, iterable));
            continue;
        }
        __wt_stat_join_init_single(&je->stats);
        /*
         * For a single compare=le/lt endpoint in any entry that may be iterated, construct a
         * companion compare=ge endpoint that will actually be iterated.
         */
        if (iterable && je->ends_next == 1 && F_ISSET(&je->ends[0], WT_CURJOIN_END_LT)) {
            origcur = je->ends[0].cursor;
            WT_ERR(__curjoin_insert_endpoint(session, je, 0, &end));
            WT_ERR(__wt_open_cursor(session, origcur->uri, (WT_CURSOR *)cjoin,
              F_ISSET(origcur, WT_CURSTD_RAW) ? raw_cfg : def_cfg, &end->cursor));
            end->flags = WT_CURJOIN_END_GT | WT_CURJOIN_END_EQ | WT_CURJOIN_END_OWN_CURSOR;
            WT_ERR(end->cursor->next(end->cursor));
            F_CLR(je, WT_CURJOIN_ENTRY_DISJUNCTION);
        }
        for (end = &je->ends[0]; end < &je->ends[je->ends_next]; end++)
            WT_ERR(__curjoin_endpoint_init_key(session, je, end));

        /*
         * Do any needed Bloom filter initialization. Ignore Bloom filters for entries that will be
         * iterated. They won't help since these entries either don't need an inclusion check or are
         * doing any needed check during the iteration.
         */
        if (!iterable && F_ISSET(je, WT_CURJOIN_ENTRY_BLOOM)) {
            if (session->txn->isolation == WT_ISO_READ_UNCOMMITTED)
                WT_ERR_MSG(session, EINVAL,
                  "join cursors with Bloom filters cannot be used with read-uncommitted isolation");
            if (je->bloom == NULL) {
                /*
                 * Look for compatible filters to be shared, pick compatible numbers for bit counts
                 * and number of hashes.
                 */
                f = je->bloom_bit_count;
                k = je->bloom_hash_count;
                for (je2 = je + 1; je2 < jeend; je2++)
                    if (F_ISSET(je2, WT_CURJOIN_ENTRY_BLOOM) && je2->count == je->count) {
                        f = WT_MAX(je2->bloom_bit_count, f);
                        k = WT_MAX(je2->bloom_hash_count, k);
                    }
                je->bloom_bit_count = f;
                je->bloom_hash_count = k;
                WT_ERR(__wt_bloom_create(session, NULL, NULL, je->count, f, k, &je->bloom));
                F_SET(je, WT_CURJOIN_ENTRY_OWN_BLOOM);
                WT_ERR(__curjoin_init_bloom(session, cjoin, je, je->bloom));
                /*
                 * Share the Bloom filter, making all config info consistent.
                 */
                for (je2 = je + 1; je2 < jeend; je2++)
                    if (F_ISSET(je2, WT_CURJOIN_ENTRY_BLOOM) && je2->count == je->count) {
                        WT_ASSERT(session, je2->bloom == NULL);
                        je2->bloom = je->bloom;
                        je2->bloom_bit_count = f;
                        je2->bloom_hash_count = k;
                    }
            } else {
                /*
                 * Create a temporary filter that we'll merge into the shared one. The Bloom
                 * parameters of the two filters must match.
                 */
                WT_ERR(__wt_bloom_create(session, NULL, NULL, je->count, je->bloom_bit_count,
                  je->bloom_hash_count, &bloom));
                WT_ERR(__curjoin_init_bloom(session, cjoin, je, bloom));
                WT_ERR(__wt_bloom_intersection(je->bloom, bloom));
                WT_ERR(__wt_bloom_close(bloom));
            }
        }
        if (!F_ISSET(cjoin, WT_CURJOIN_DISJUNCTION))
            iterable = false;
    }
    F_SET(cjoin, WT_CURJOIN_INITIALIZED);

err:
    __wt_free(session, mainbuf);
    return (ret);
}

/*
 * __curjoin_insert_endpoint --
 *     Insert a new entry into the endpoint array for the join entry.
 */
static int
__curjoin_insert_endpoint(WT_SESSION_IMPL *session, WT_CURSOR_JOIN_ENTRY *entry, u_int pos,
  WT_CURSOR_JOIN_ENDPOINT **newendp)
{
    WT_CURSOR_JOIN_ENDPOINT *newend;

    WT_RET(__wt_realloc_def(session, &entry->ends_allocated, entry->ends_next + 1, &entry->ends));
    newend = &entry->ends[pos];
    memmove(newend + 1, newend, (entry->ends_next - pos) * sizeof(WT_CURSOR_JOIN_ENDPOINT));
    memset(newend, 0, sizeof(WT_CURSOR_JOIN_ENDPOINT));
    entry->ends_next++;
    *newendp = newend;

    return (0);
}

/*
 * __curjoin_next --
 *     WT_CURSOR::next for join cursors.
 */
static int
__curjoin_next(WT_CURSOR *cursor)
{
    WT_CURSOR *c;
    WT_CURSOR_JOIN *cjoin;
    WT_CURSOR_JOIN_ITER *iter;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    int tret;

    cjoin = (WT_CURSOR_JOIN *)cursor;

    JOINABLE_CURSOR_API_CALL(cursor, session, next, NULL);

    if (F_ISSET(cjoin, WT_CURJOIN_ERROR))
        WT_ERR_MSG(session, WT_ERROR, "join cursor encountered previous error");
    if (!F_ISSET(cjoin, WT_CURJOIN_INITIALIZED))
        WT_ERR(__curjoin_init_next(session, cjoin, true));
    if (cjoin->iter == NULL)
        WT_ERR(__curjoin_iter_init(session, cjoin, &cjoin->iter));
    iter = cjoin->iter;
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    while ((ret = __curjoin_iter_next(iter, cursor)) == 0) {
        if ((ret = __curjoin_entries_in_range(session, cjoin, iter->curkey, iter)) != WT_NOTFOUND)
            break;
    }
    iter->positioned = (ret == 0);
    WT_ERR_NOTFOUND_OK(ret, true);

    if (ret == 0) {
        /*
         * Position the 'main' cursor, this will be used to retrieve values from the cursor join.
         * The key we have is raw, but the main cursor may not be raw.
         */
        c = cjoin->main;
        __wt_cursor_set_raw_key(c, iter->curkey);

        /*
         * A failed search is not expected, convert WT_NOTFOUND into a generic error.
         */
        iter->entry->stats.main_access++;
        if ((ret = c->search(c)) != 0) {
            if (ret == WT_NOTFOUND)
                ret = WT_ERROR;
            WT_ERR_MSG(session, ret, "join cursor failed search");
        }

        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    } else if (ret == WT_NOTFOUND && (tret = __curjoin_iter_close_all(iter)) != 0)
        WT_ERR(tret);

    if (0) {
err:
        F_SET(cjoin, WT_CURJOIN_ERROR);
    }
    API_END_RET(session, ret);
}

/*
 * __curjoin_open_main --
 *     For the given index, open the main file with a projection that is the index keys.
 */
static int
__curjoin_open_main(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin, WT_CURSOR_JOIN_ENTRY *entry)
{
    WT_DECL_RET;
    WT_INDEX *idx;
    size_t len, newsize;
    char *main_uri, *newformat;
    const char *raw_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "raw", NULL};

    main_uri = newformat = NULL;
    idx = entry->index;

    newsize = strlen(cjoin->table->iface.name) + idx->colconf.len + 1;
    WT_ERR(__wt_calloc(session, 1, newsize, &main_uri));
    WT_ERR(__wt_snprintf(main_uri, newsize, "%s%.*s", cjoin->table->iface.name,
      (int)idx->colconf.len, idx->colconf.str));
    WT_ERR(__wt_open_cursor(session, main_uri, (WT_CURSOR *)cjoin, raw_cfg, &entry->main));
    if (idx->extractor == NULL) {
        /*
         * Add no-op padding so trailing 'u' formats are not transformed to 'U'. This matches what
         * happens in the index. We don't do this when we have an extractor, extractors already use
         * the padding byte trick.
         */
        len = strlen(entry->main->value_format) + 3;
        WT_ERR(__wt_calloc(session, len, 1, &newformat));
        WT_ERR(__wt_snprintf(newformat, len, "%s0x", entry->main->value_format));
        __wt_free(session, entry->main->value_format);
        entry->main->value_format = newformat;
        newformat = NULL;
    }

err:
    __wt_free(session, main_uri);
    __wt_free(session, newformat);
    return (ret);
}

/*
 * __curjoin_pack_recno --
 *     Pack the given recno into a buffer; prepare an item referencing it.
 */
static int
__curjoin_pack_recno(
  WT_SESSION_IMPL *session, uint64_t r, uint8_t *buf, size_t bufsize, WT_ITEM *item)
{
    WT_SESSION *wtsession;
    size_t sz;

    wtsession = (WT_SESSION *)session;
    WT_RET(wiredtiger_struct_size(wtsession, &sz, "r", r));
    WT_ASSERT(session, sz < bufsize);
    WT_RET(wiredtiger_struct_pack(wtsession, buf, bufsize, "r", r));
    item->size = sz;
    item->data = buf;
    return (0);
}

/*
 * __curjoin_reset --
 *     WT_CURSOR::reset for join cursors.
 */
static int
__curjoin_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_JOIN *cjoin;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cjoin = (WT_CURSOR_JOIN *)cursor;

    JOINABLE_CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);

    if (cjoin->iter != NULL)
        WT_ERR(__curjoin_iter_reset(cjoin->iter));

err:
    API_END_RET(session, ret);
}

/*
 * __curjoin_split_key --
 *     Copy the primary key from a cursor (either main table or index) to another cursor. When
 *     copying from an index file, the index key is also returned.
 */
static int
__curjoin_split_key(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin, WT_ITEM *idxkey,
  WT_CURSOR *tocur, WT_CURSOR *fromcur, const char *repack_fmt, bool isindex)
{
    WT_CURSOR *firstcg_cur;
    WT_CURSOR_INDEX *cindex;
    WT_ITEM *keyp;
    const uint8_t *p;

    if (isindex) {
        cindex = ((WT_CURSOR_INDEX *)fromcur);
        /*
         * Repack tells us where the index key ends; advance past that to get where the raw primary
         * key starts.
         */
        WT_RET(__wt_struct_repack(session, cindex->child->key_format,
          repack_fmt != NULL ? repack_fmt : cindex->iface.key_format, &cindex->child->key, idxkey));
        WT_ASSERT(session, cindex->child->key.size > idxkey->size);
        tocur->key.data = (uint8_t *)idxkey->data + idxkey->size;
        tocur->key.size = cindex->child->key.size - idxkey->size;
        if (WT_CURSOR_RECNO(tocur)) {
            p = (const uint8_t *)tocur->key.data;
            WT_RET(__wt_vunpack_uint(&p, tocur->key.size, &tocur->recno));
        } else
            tocur->recno = 0;
    } else {
        firstcg_cur = ((WT_CURSOR_TABLE *)fromcur)->cg_cursors[0];
        keyp = &firstcg_cur->key;
        if (WT_CURSOR_RECNO(tocur)) {
            WT_ASSERT(session, keyp->size == sizeof(uint64_t));
            tocur->recno = *(uint64_t *)keyp->data;
            WT_RET(__curjoin_pack_recno(
              session, tocur->recno, cjoin->recno_buf, sizeof(cjoin->recno_buf), &tocur->key));
        } else {
            WT_ITEM_SET(tocur->key, *keyp);
            tocur->recno = 0;
        }
        idxkey->data = NULL;
        idxkey->size = 0;
    }
    return (0);
}

/*
 * __wt_curjoin_open --
 *     Initialize a join cursor. Join cursors are read-only.
 */
int
__wt_curjoin_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __curjoin_get_key, /* get-key */
      __curjoin_get_value,                          /* get-value */
      __wt_cursor_get_raw_key_value_notsup,         /* get-raw-key-value */
      __wt_cursor_set_key_notsup,                   /* set-key */
      __wt_cursor_set_value_notsup,                 /* set-value */
      __wt_cursor_compare_notsup,                   /* compare */
      __wt_cursor_equals_notsup,                    /* equals */
      __curjoin_next,                               /* next */
      __wt_cursor_notsup,                           /* prev */
      __curjoin_reset,                              /* reset */
      __wt_cursor_notsup,                           /* search */
      __wt_cursor_search_near_notsup,               /* search-near */
      __wt_cursor_notsup,                           /* insert */
      __wt_cursor_modify_notsup,                    /* modify */
      __wt_cursor_notsup,                           /* update */
      __wt_cursor_notsup,                           /* remove */
      __wt_cursor_notsup,                           /* reserve */
      __wt_cursor_config_notsup,                    /* reconfigure */
      __wt_cursor_notsup,                           /* largest_key */
      __wt_cursor_config_notsup,                    /* bound */
      __wt_cursor_notsup,                           /* cache */
      __wt_cursor_reopen_notsup,                    /* reopen */
      __wt_cursor_checkpoint_id,                    /* checkpoint ID */
      __curjoin_close);                             /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_JOIN *cjoin;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_TABLE *table;
    size_t size;
    const char *tablename, *columns;

    WT_STATIC_ASSERT(offsetof(WT_CURSOR_JOIN, iface) == 0);

    if (owner != NULL)
        WT_RET_MSG(session, EINVAL, "unable to initialize a join cursor with existing owner");

    tablename = uri;
    if (!WT_PREFIX_SKIP(tablename, "join:table:"))
        return (__wt_unexpected_object_type(session, uri, "join:table:"));

    columns = strchr(tablename, '(');
    if (columns == NULL)
        size = strlen(tablename);
    else
        size = WT_PTRDIFF(columns, tablename);
    WT_RET(__wt_schema_get_table(session, tablename, size, false, 0, &table));

    WT_RET(__wt_calloc_one(session, &cjoin));
    cursor = (WT_CURSOR *)cjoin;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->key_format = table->key_format;
    cursor->value_format = table->value_format;

    cjoin->table = table;

    /* Handle projections. */
    WT_ERR(__wt_scr_alloc(session, 0, &tmp));
    if (columns != NULL) {
        WT_ERR(__wt_struct_reformat(session, table, columns, strlen(columns), NULL, false, tmp));
        WT_ERR(__wt_strndup(session, tmp->data, tmp->size, &cursor->value_format));
        WT_ERR(__wt_strdup(session, columns, &cjoin->projection));
    }

    WT_ERR(__wt_cursor_init(cursor, uri, owner, cfg, cursorp));

    if (0) {
err:
        WT_TRET(__curjoin_close(cursor));
        *cursorp = NULL;
    }

    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_curjoin_join --
 *     Add a new join to a join cursor.
 */
int
__wt_curjoin_join(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin, WT_INDEX *idx,
  WT_CURSOR *ref_cursor, uint8_t flags, uint8_t range, uint64_t count, uint32_t bloom_bit_count,
  uint32_t bloom_hash_count)
{
    WT_CURSOR_INDEX *cindex;
    WT_CURSOR_JOIN *child;
    WT_CURSOR_JOIN_ENDPOINT *end;
    WT_CURSOR_JOIN_ENTRY *entry;
    size_t len;
    uint8_t endrange;
    u_int i, ins, nonbloom;
    bool hasins, needbloom, nested, range_eq;

    entry = NULL;
    hasins = needbloom = false;
    ins = nonbloom = 0; /* -Wuninitialized */

    if (cjoin->entries_next == 0) {
        if (LF_ISSET(WT_CURJOIN_ENTRY_DISJUNCTION))
            F_SET(cjoin, WT_CURJOIN_DISJUNCTION);
    } else if (LF_ISSET(WT_CURJOIN_ENTRY_DISJUNCTION) && !F_ISSET(cjoin, WT_CURJOIN_DISJUNCTION))
        WT_RET_MSG(session, EINVAL, "operation=or does not match previous operation=and");
    else if (!LF_ISSET(WT_CURJOIN_ENTRY_DISJUNCTION) && F_ISSET(cjoin, WT_CURJOIN_DISJUNCTION))
        WT_RET_MSG(session, EINVAL, "operation=and does not match previous operation=or");

    nested = WT_PREFIX_MATCH(ref_cursor->uri, "join:");
    if (!nested)
        for (i = 0; i < cjoin->entries_next; i++) {
            if (cjoin->entries[i].index == idx && cjoin->entries[i].subjoin == NULL) {
                entry = &cjoin->entries[i];
                break;
            }
            if (!needbloom && i > 0 && !F_ISSET(&cjoin->entries[i], WT_CURJOIN_ENTRY_BLOOM)) {
                needbloom = true;
                nonbloom = i;
            }
        }
    else {
        if (LF_ISSET(WT_CURJOIN_ENTRY_BLOOM))
            WT_RET_MSG(session, EINVAL, "Bloom filters cannot be used with subjoins");
    }

    if (entry == NULL) {
        WT_RET(__wt_realloc_def(
          session, &cjoin->entries_allocated, cjoin->entries_next + 1, &cjoin->entries));
        if (LF_ISSET(WT_CURJOIN_ENTRY_BLOOM) && needbloom) {
            /*
             * Reorder the list so that after the first entry, the Bloom filtered entries come next,
             * followed by the non-Bloom entries. Once the Bloom filters are built, determining
             * membership via Bloom is faster than without Bloom, so we can answer membership
             * questions more quickly, and with less I/O, with the Bloom entries first.
             */
            entry = &cjoin->entries[nonbloom];
            memmove(
              entry + 1, entry, (cjoin->entries_next - nonbloom) * sizeof(WT_CURSOR_JOIN_ENTRY));
            memset(entry, 0, sizeof(WT_CURSOR_JOIN_ENTRY));
        } else
            entry = &cjoin->entries[cjoin->entries_next];
        entry->index = idx;
        entry->flags = flags;
        entry->count = count;
        entry->bloom_bit_count = bloom_bit_count;
        entry->bloom_hash_count = bloom_hash_count;
        ++cjoin->entries_next;
    } else {
        /* Merge the join into an existing entry for this index */
        if (count != 0 && entry->count != 0 && entry->count != count)
            WT_RET_MSG(session, EINVAL,
              "count=%" PRIu64 " does not match previous count=%" PRIu64 " for this index", count,
              entry->count);
        if (LF_MASK(WT_CURJOIN_ENTRY_BLOOM) != F_MASK(entry, WT_CURJOIN_ENTRY_BLOOM))
            WT_RET_MSG(session, EINVAL, "join has incompatible strategy values for the same index");
        if (LF_MASK(WT_CURJOIN_ENTRY_FALSE_POSITIVES) !=
          F_MASK(entry, WT_CURJOIN_ENTRY_FALSE_POSITIVES))
            WT_RET_MSG(session, EINVAL,
              "join has incompatible bloom_false_positives values for the same index");

        /*
         * Check against other comparisons (we call them endpoints)
         * already set up for this index.
         * We allow either:
         *   - one or more "eq" (with disjunction)
         *   - exactly one "eq" (with conjunction)
         *   - exactly one of "gt" or "ge" (conjunction or disjunction)
         *   - exactly one of "lt" or "le" (conjunction or disjunction)
         *   - one of "gt"/"ge" along with one of "lt"/"le"
         *         (currently restricted to conjunction).
         *
         * Some other combinations, although expressible either do
         * not make sense (X == 3 AND X == 5) or are reducible (X <
         * 7 AND X < 9).  Other specific cases of (X < 7 OR X > 15)
         * or (X == 4 OR X > 15) make sense but we don't handle yet.
         */
        for (i = 0; i < entry->ends_next; i++) {
            end = &entry->ends[i];
            range_eq = (range == WT_CURJOIN_END_EQ);
            endrange = WT_CURJOIN_END_RANGE(end);
            if ((F_ISSET(end, WT_CURJOIN_END_GT) &&
                  ((range & WT_CURJOIN_END_GT) != 0 || range_eq)) ||
              (F_ISSET(end, WT_CURJOIN_END_LT) && ((range & WT_CURJOIN_END_LT) != 0 || range_eq)) ||
              (endrange == WT_CURJOIN_END_EQ &&
                (range & (WT_CURJOIN_END_LT | WT_CURJOIN_END_GT)) != 0))
                WT_RET_MSG(session, EINVAL, "join has overlapping ranges");
            if (range == WT_CURJOIN_END_EQ && endrange == WT_CURJOIN_END_EQ &&
              !F_ISSET(entry, WT_CURJOIN_ENTRY_DISJUNCTION))
                WT_RET_MSG(session, EINVAL, "compare=eq can only be combined using operation=or");

            /*
             * Sort "gt"/"ge" to the front, followed by any number of "eq", and finally "lt"/"le".
             */
            if (!hasins &&
              ((range & WT_CURJOIN_END_GT) != 0 ||
                (range == WT_CURJOIN_END_EQ && endrange != WT_CURJOIN_END_EQ &&
                  !F_ISSET(end, WT_CURJOIN_END_GT)))) {
                ins = i;
                hasins = true;
            }
        }
        /* All checks completed, merge any new configuration now */
        entry->count = count;
        entry->bloom_bit_count = WT_MAX(entry->bloom_bit_count, bloom_bit_count);
        entry->bloom_hash_count = WT_MAX(entry->bloom_hash_count, bloom_hash_count);
    }
    if (nested) {
        child = (WT_CURSOR_JOIN *)ref_cursor;
        entry->subjoin = child;
        child->parent = cjoin;
    } else {
        WT_RET(__curjoin_insert_endpoint(session, entry, hasins ? ins : entry->ends_next, &end));
        end->cursor = ref_cursor;
        F_SET(end, range);

        if (entry->main == NULL && idx != NULL) {
            /*
             * Open the main file with a projection of the indexed columns.
             */
            WT_RET(__curjoin_open_main(session, cjoin, entry));

            /*
             * When we are repacking index keys to remove the primary key, we never want to
             * transform trailing 'u'. Use no-op padding to force this.
             */
            cindex = (WT_CURSOR_INDEX *)ref_cursor;
            len = strlen(cindex->iface.key_format) + 3;
            WT_RET(__wt_calloc(session, len, 1, &entry->repack_format));
            WT_RET(__wt_snprintf(entry->repack_format, len, "%s0x", cindex->iface.key_format));
        }
    }
    return (0);
}
