/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __curjoin_insert_endpoint(WT_SESSION_IMPL *,
    WT_CURSOR_JOIN_ENTRY *, u_int, WT_CURSOR_JOIN_ENDPOINT **);

/*
 * __curjoin_entry_iter_init --
 *	Initialize an iteration for the index managed by a join entry.
 *
 */
static int
__curjoin_entry_iter_init(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin,
    WT_CURSOR_JOIN_ENTRY *entry, WT_CURSOR_JOIN_ITER **iterp)
{
	WT_CURSOR *to_dup;
	WT_DECL_RET;
	const char *raw_cfg[] = { WT_CONFIG_BASE(
	    session, WT_SESSION_open_cursor), "raw", NULL };
	const char *def_cfg[] = { WT_CONFIG_BASE(
	    session, WT_SESSION_open_cursor), NULL };
	const char *urimain, **config;
	char *mainbuf, *uri;
	WT_CURSOR_JOIN_ITER *iter;
	size_t size;

	iter = NULL;
	mainbuf = uri = NULL;
	to_dup = entry->ends[0].cursor;

	if (F_ISSET((WT_CURSOR *)cjoin, WT_CURSTD_RAW))
		config = &raw_cfg[0];
	else
		config = &def_cfg[0];

	size = strlen(to_dup->internal_uri) + 3;
	WT_ERR(__wt_calloc(session, size, 1, &uri));
	snprintf(uri, size, "%s()", to_dup->internal_uri);
	urimain = cjoin->table->name;
	if (cjoin->projection != NULL) {
		size = strlen(urimain) + strlen(cjoin->projection) + 1;
		WT_ERR(__wt_calloc(session, size, 1, &mainbuf));
		snprintf(mainbuf, size, "%s%s", urimain, cjoin->projection);
		urimain = mainbuf;
	}

	WT_ERR(__wt_calloc_one(session, &iter));
	WT_ERR(__wt_open_cursor(session, uri, (WT_CURSOR *)cjoin, config,
	    &iter->cursor));
	WT_ERR(__wt_cursor_dup_position(to_dup, iter->cursor));
	WT_ERR(__wt_open_cursor(session, urimain, (WT_CURSOR *)cjoin, config,
	    &iter->main));
	iter->cjoin = cjoin;
	iter->session = session;
	iter->entry = entry;
	iter->positioned = false;
	iter->isequal = (entry->ends_next == 1 &&
	    WT_CURJOIN_END_RANGE(&entry->ends[0]) == WT_CURJOIN_END_EQ);
	*iterp = iter;

	if (0) {
err:		__wt_free(session, iter);
	}
	__wt_free(session, mainbuf);
	__wt_free(session, uri);
	return (ret);
}

/*
 * __curjoin_pack_recno --
 *	Pack the given recno into a buffer; prepare an item referencing it.
 *
 */
static int
__curjoin_pack_recno(WT_SESSION_IMPL *session, uint64_t r, uint8_t *buf,
    size_t bufsize, WT_ITEM *item)
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
 * __curjoin_split_key --
 *	Copy the primary key from a cursor (either main table or index)
 *	to another cursor.  When copying from an index file, the index
 *	key is also returned.
 *
 */
static int
__curjoin_split_key(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin,
    WT_ITEM *idxkey, WT_CURSOR *tocur, WT_CURSOR *fromcur,
    const char *repack_fmt, bool isindex)
{
	WT_CURSOR *firstcg_cur;
	WT_CURSOR_INDEX *cindex;
	WT_ITEM *keyp;
	const uint8_t *p;

	if (isindex) {
		cindex = ((WT_CURSOR_INDEX *)fromcur);
		/*
		 * Repack tells us where the index key ends; advance past
		 * that to get where the raw primary key starts.
		 */
		WT_RET(__wt_struct_repack(session, cindex->child->key_format,
		    repack_fmt != NULL ? repack_fmt : cindex->iface.key_format,
		    &cindex->child->key, idxkey));
		WT_ASSERT(session, cindex->child->key.size > idxkey->size);
		tocur->key.data = (uint8_t *)idxkey->data + idxkey->size;
		tocur->key.size = cindex->child->key.size - idxkey->size;
		if (WT_CURSOR_RECNO(tocur)) {
			p = (const uint8_t *)tocur->key.data;
			WT_RET(__wt_vunpack_uint(&p, tocur->key.size,
			    &tocur->recno));
		} else
			tocur->recno = 0;
	} else {
		firstcg_cur = ((WT_CURSOR_TABLE *)fromcur)->cg_cursors[0];
		keyp = &firstcg_cur->key;
		if (WT_CURSOR_RECNO(tocur)) {
			WT_ASSERT(session, keyp->size == sizeof(uint64_t));
			tocur->recno = *(uint64_t *)keyp->data;
			WT_RET(__curjoin_pack_recno(session, tocur->recno,
			    cjoin->recno_buf, sizeof(cjoin->recno_buf),
			    &tocur->key));
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
 * __curjoin_entry_iter_next --
 *	Get the next item in an iteration.
 *
 */
static int
__curjoin_entry_iter_next(WT_CURSOR_JOIN_ITER *iter, WT_CURSOR *cursor)
{
	if (iter->positioned)
		WT_RET(iter->cursor->next(iter->cursor));
	else
		iter->positioned = true;

	/*
	 * Set our key to the primary key, we'll also need this
	 * to check membership.
	 */
	WT_RET(__curjoin_split_key(iter->session, iter->cjoin, &iter->idxkey,
	    cursor, iter->cursor, iter->entry->repack_format,
	    iter->entry->index != NULL));
	iter->curkey = &cursor->key;
	iter->entry->stats.actual_count++;
	iter->entry->stats.accesses++;
	return (0);
}

/*
 * __curjoin_entry_iter_reset --
 *	Reset an iteration to the starting point.
 *
 */
static int
__curjoin_entry_iter_reset(WT_CURSOR_JOIN_ITER *iter)
{
	if (iter->positioned) {
		WT_RET(iter->cursor->reset(iter->cursor));
		WT_RET(iter->main->reset(iter->main));
		WT_RET(__wt_cursor_dup_position(
		    iter->cjoin->entries[0].ends[0].cursor, iter->cursor));
		iter->positioned = false;
		iter->entry->stats.actual_count = 0;
	}
	return (0);
}

/*
 * __curjoin_entry_iter_ready --
 *	The iterator is positioned.
 *
 */
static bool
__curjoin_entry_iter_ready(WT_CURSOR_JOIN_ITER *iter)
{
	return (iter->positioned);
}

/*
 * __curjoin_entry_iter_close --
 *	Close the iteration, release resources.
 *
 */
static int
__curjoin_entry_iter_close(WT_CURSOR_JOIN_ITER *iter)
{
	WT_DECL_RET;

	if (iter->cursor != NULL)
		WT_TRET(iter->cursor->close(iter->cursor));
	if (iter->main != NULL)
		WT_TRET(iter->main->close(iter->main));
	__wt_free(iter->session, iter);

	return (ret);
}

/*
 * __curjoin_get_key --
 *	WT_CURSOR->get_key for join cursors.
 */
static int
__curjoin_get_key(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_JOIN *cjoin;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	cjoin = (WT_CURSOR_JOIN *)cursor;

	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, get_key, NULL);

	if (!F_ISSET(cjoin, WT_CURJOIN_INITIALIZED) ||
	    !__curjoin_entry_iter_ready(cjoin->iter))
		WT_ERR_MSG(session, EINVAL,
		    "join cursor must be advanced with next()");
	WT_ERR(__wt_cursor_get_keyv(cursor, cursor->flags, ap));

err:	va_end(ap);
	API_END_RET(session, ret);
}

/*
 * __curjoin_get_value --
 *	WT_CURSOR->get_value for join cursors.
 */
static int
__curjoin_get_value(WT_CURSOR *cursor, ...)
{
	WT_CURSOR_JOIN *cjoin;
	WT_CURSOR_JOIN_ITER *iter;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	cjoin = (WT_CURSOR_JOIN *)cursor;
	iter = cjoin->iter;

	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, get_value, NULL);

	if (!F_ISSET(cjoin, WT_CURJOIN_INITIALIZED) ||
	    !__curjoin_entry_iter_ready(iter))
		WT_ERR_MSG(session, EINVAL,
		    "join cursor must be advanced with next()");

	WT_ERR(__wt_curtable_get_valuev(iter->main, ap));

err:	va_end(ap);
	API_END_RET(session, ret);
}

/*
 * __curjoin_init_bloom --
 *	Populate Bloom filters
 */
static int
__curjoin_init_bloom(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin,
    WT_CURSOR_JOIN_ENTRY *entry, WT_BLOOM *bloom)
{
	WT_COLLATOR *collator;
	WT_CURSOR *c;
	WT_CURSOR_JOIN_ENDPOINT *end, *endmax;
	WT_DECL_RET;
	WT_DECL_ITEM(uribuf);
	WT_ITEM curkey, curvalue;
	const char *raw_cfg[] = { WT_CONFIG_BASE(
	    session, WT_SESSION_open_cursor), "raw", NULL };
	const char *uri;
	size_t size;
	int cmp, skip;

	c = NULL;
	skip = 0;

	if (entry->index != NULL)
		/*
		 * Open the raw index.  We're avoiding any references
		 * to the main table, they may be expensive.
		 */
		uri = entry->index->source;
	else {
		/*
		 * For joins on the main table, we just need the primary
		 * key for comparison, we don't need any values.
		 */
		size = strlen(cjoin->table->name) + 3;
		WT_ERR(__wt_scr_alloc(session, size, &uribuf));
		WT_ERR(__wt_buf_fmt(session, uribuf, "%s()",
		    cjoin->table->name));
		uri = uribuf->data;
	}
	WT_ERR(__wt_open_cursor(session, uri, &cjoin->iface, raw_cfg, &c));

	/* Initially position the cursor if necessary. */
	endmax = &entry->ends[entry->ends_next];
	if ((end = &entry->ends[0]) < endmax) {
		if (F_ISSET(end, WT_CURJOIN_END_GT) ||
		    WT_CURJOIN_END_RANGE(end) == WT_CURJOIN_END_EQ) {
			WT_ERR(__wt_cursor_dup_position(end->cursor, c));
			if (WT_CURJOIN_END_RANGE(end) == WT_CURJOIN_END_GE)
				skip = 1;
		} else if (F_ISSET(end, WT_CURJOIN_END_LT)) {
			if ((ret = c->next(c)) == WT_NOTFOUND)
				goto done;
			WT_ERR(ret);
		} else
			WT_ERR(__wt_illegal_value(session, NULL));
	}
	collator = (entry->index == NULL) ? NULL : entry->index->collator;
	while (ret == 0) {
		WT_ERR(c->get_key(c, &curkey));
		if (entry->index != NULL) {
			/*
			 * Repack so it's comparable to the
			 * reference endpoints.
			 */
			WT_ERR(__wt_struct_repack(session,
			    c->key_format,
			    (entry->repack_format != NULL ?
			    entry->repack_format : entry->index->idxkey_format),
			    &c->key, &curkey));
		}
		for (end = &entry->ends[skip]; end < endmax; end++) {
			WT_ERR(__wt_compare(session, collator, &curkey,
			    &end->key, &cmp));
			if (!F_ISSET(end, WT_CURJOIN_END_LT)) {
				if (cmp < 0 || (cmp == 0 &&
				    !F_ISSET(end, WT_CURJOIN_END_EQ)))
					goto advance;
				if (cmp > 0) {
					if (F_ISSET(end, WT_CURJOIN_END_GT))
						skip = 1;
					else
						goto done;
				}
			} else {
				if (cmp > 0 || (cmp == 0 &&
				    !F_ISSET(end, WT_CURJOIN_END_EQ)))
					goto done;
			}
		}
		if (entry->index != NULL) {
			curvalue.data =
			    (unsigned char *)curkey.data + curkey.size;
			WT_ASSERT(session, c->key.size > curkey.size);
			curvalue.size = c->key.size - curkey.size;
		}
		else
			WT_ERR(c->get_key(c, &curvalue));
		WT_ERR(__wt_bloom_insert(bloom, &curvalue));
		entry->stats.actual_count++;
advance:
		if ((ret = c->next(c)) == WT_NOTFOUND)
			break;
	}
done:
	WT_ERR_NOTFOUND_OK(ret);

err:	if (c != NULL)
		WT_TRET(c->close(c));
	__wt_scr_free(session, &uribuf);
	return (ret);
}

/*
 * __curjoin_endpoint_init_key --
 *	Set the key in the reference endpoint.
 */
static int
__curjoin_endpoint_init_key(WT_SESSION_IMPL *session,
    WT_CURSOR_JOIN_ENTRY *entry, WT_CURSOR_JOIN_ENDPOINT *endpoint)
{
	WT_CURSOR *cursor;
	WT_CURSOR_INDEX *cindex;
	WT_ITEM *k;
	uint64_t r;

	if ((cursor = endpoint->cursor) != NULL) {
		if (entry->index != NULL) {
			/* Extract and save the index's logical key. */
			cindex = (WT_CURSOR_INDEX *)endpoint->cursor;
			WT_RET(__wt_struct_repack(session,
			    cindex->child->key_format,
			    (entry->repack_format != NULL ?
			    entry->repack_format : cindex->iface.key_format),
			    &cindex->child->key, &endpoint->key));
		} else {
			k = &((WT_CURSOR_TABLE *)cursor)->cg_cursors[0]->key;
			if (WT_CURSOR_RECNO(cursor)) {
				r = *(uint64_t *)k->data;
				WT_RET(__curjoin_pack_recno(session, r,
				    endpoint->recno_buf,
				    sizeof(endpoint->recno_buf),
				    &endpoint->key));
			}
			else
				endpoint->key = *k;
		}
	}
	return (0);
}

/*
 * __curjoin_init_iter --
 *	Initialize before any iteration.
 */
static int
__curjoin_init_iter(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin)
{
	WT_BLOOM *bloom;
	WT_DECL_RET;
	WT_CURSOR *origcur;
	WT_CURSOR_JOIN_ENTRY *je, *jeend, *je2;
	WT_CURSOR_JOIN_ENDPOINT *end;
	const char *def_cfg[] = { WT_CONFIG_BASE(
	    session, WT_SESSION_open_cursor), NULL };
	const char *raw_cfg[] = { WT_CONFIG_BASE(
	    session, WT_SESSION_open_cursor), "raw", NULL };
	uint32_t f, k;

	if (cjoin->entries_next == 0)
		WT_RET_MSG(session, EINVAL,
		    "join cursor has not yet been joined with any other "
		    "cursors");

	je = &cjoin->entries[0];
	jeend = &cjoin->entries[cjoin->entries_next];

	/*
	 * For a single compare=le endpoint in the first iterated entry,
	 * construct a companion compare=ge endpoint that will actually
	 * be iterated.
	 */
	if (((je = cjoin->entries) != jeend) &&
	    je->ends_next == 1 && F_ISSET(&je->ends[0], WT_CURJOIN_END_LT)) {
		origcur = je->ends[0].cursor;
		WT_RET(__curjoin_insert_endpoint(session, je, 0, &end));
		WT_RET(__wt_open_cursor(session, origcur->uri,
		    (WT_CURSOR *)cjoin,
		    F_ISSET(origcur, WT_CURSTD_RAW) ? raw_cfg : def_cfg,
		    &end->cursor));
		WT_RET(end->cursor->next(end->cursor));
		end->flags = WT_CURJOIN_END_GT | WT_CURJOIN_END_EQ |
		    WT_CURJOIN_END_OWN_CURSOR;
	}
	WT_RET(__curjoin_entry_iter_init(session, cjoin, je, &cjoin->iter));

	for (je = cjoin->entries; je < jeend; je++) {
		__wt_stat_join_init_single(&je->stats);
		for (end = &je->ends[0]; end < &je->ends[je->ends_next];
		     end++)
			WT_RET(__curjoin_endpoint_init_key(session, je, end));

		/*
		 * The first entry is iterated as the 'outermost' cursor.
		 * For the common GE case, we don't have to test against
		 * the left reference key, we know it will be true since
		 * the btree is ordered.
		 */
		if (je == cjoin->entries && je->ends[0].flags ==
		    (WT_CURJOIN_END_GT | WT_CURJOIN_END_EQ))
			F_SET(cjoin, WT_CURJOIN_SKIP_FIRST_LEFT);

		if (F_ISSET(je, WT_CURJOIN_ENTRY_BLOOM)) {
			if (session->txn.isolation == WT_ISO_READ_UNCOMMITTED)
			       WT_RET_MSG(session, EINVAL,
				   "join cursors with Bloom filters cannot be "
				   "used with read-uncommitted isolation");
			if (je->bloom == NULL) {
				/*
				 * Look for compatible filters to be shared,
				 * pick compatible numbers for bit counts
				 * and number of hashes.
				 */
				f = je->bloom_bit_count;
				k = je->bloom_hash_count;
				for (je2 = je + 1; je2 < jeend; je2++)
					if (F_ISSET(je2,
					    WT_CURJOIN_ENTRY_BLOOM) &&
					    je2->count == je->count) {
						f = WT_MAX(
						    je2->bloom_bit_count, f);
						k = WT_MAX(
						    je2->bloom_hash_count, k);
					}
				je->bloom_bit_count = f;
				je->bloom_hash_count = k;
				WT_RET(__wt_bloom_create(session, NULL,
				    NULL, je->count, f, k, &je->bloom));
				F_SET(je, WT_CURJOIN_ENTRY_OWN_BLOOM);
				WT_RET(__curjoin_init_bloom(session, cjoin,
				    je, je->bloom));
				/*
				 * Share the Bloom filter, making all
				 * config info consistent.
				 */
				for (je2 = je + 1; je2 < jeend; je2++)
					if (F_ISSET(je2,
					    WT_CURJOIN_ENTRY_BLOOM) &&
					    je2->count == je->count) {
						WT_ASSERT(session,
						    je2->bloom == NULL);
						je2->bloom = je->bloom;
						je2->bloom_bit_count = f;
						je2->bloom_hash_count = k;
					}
			} else {
				/*
				 * Create a temporary filter that we'll
				 * merge into the shared one.  The Bloom
				 * parameters of the two filters must match.
				 */
				WT_RET(__wt_bloom_create(session, NULL,
				    NULL, je->count, je->bloom_bit_count,
				    je->bloom_hash_count, &bloom));
				WT_RET(__curjoin_init_bloom(session, cjoin,
				    je, bloom));
				WT_RET(__wt_bloom_intersection(je->bloom,
				    bloom));
				WT_RET(__wt_bloom_close(bloom));
			}
		}
	}

	F_SET(cjoin, WT_CURJOIN_INITIALIZED);
	return (ret);
}

/*
 * __curjoin_entry_in_range --
 *	Check if a key is in the range specified by the entry, returning
 *	WT_NOTFOUND if not.
 */
static int
__curjoin_entry_in_range(WT_SESSION_IMPL *session, WT_CURSOR_JOIN_ENTRY *entry,
    WT_ITEM *curkey, bool skip_left)
{
	WT_COLLATOR *collator;
	WT_CURSOR_JOIN_ENDPOINT *end, *endmax;
	int cmp;

	collator = (entry->index != NULL) ? entry->index->collator : NULL;
	endmax = &entry->ends[entry->ends_next];
	for (end = &entry->ends[skip_left ? 1 : 0]; end < endmax; end++) {
		WT_RET(__wt_compare(session, collator, curkey, &end->key,
		    &cmp));
		if (!F_ISSET(end, WT_CURJOIN_END_LT)) {
			if (cmp < 0 ||
			    (cmp == 0 &&
			    !F_ISSET(end, WT_CURJOIN_END_EQ)) ||
			    (cmp > 0 && !F_ISSET(end, WT_CURJOIN_END_GT)))
				WT_RET(WT_NOTFOUND);
		} else {
			if (cmp > 0 ||
			    (cmp == 0 &&
			    !F_ISSET(end, WT_CURJOIN_END_EQ)) ||
			    (cmp < 0 && !F_ISSET(end, WT_CURJOIN_END_LT)))
				WT_RET(WT_NOTFOUND);
		}
	}
	return (0);
}

typedef struct {
	WT_CURSOR iface;
	WT_CURSOR_JOIN_ENTRY *entry;
	bool ismember;
} WT_CURJOIN_EXTRACTOR;

/*
 * __curjoin_extract_insert --
 *	Handle a key produced by a custom extractor.
 */
static int
__curjoin_extract_insert(WT_CURSOR *cursor) {
	WT_CURJOIN_EXTRACTOR *cextract;
	WT_DECL_RET;
	WT_ITEM ikey;
	WT_SESSION_IMPL *session;

	cextract = (WT_CURJOIN_EXTRACTOR *)cursor;
	/*
	 * This insert method may be called multiple times during a single
	 * extraction.  If we already have a definitive answer to the
	 * membership question, exit early.
	 */
	if (cextract->ismember)
		return (0);

	session = (WT_SESSION_IMPL *)cursor->session;

	WT_ITEM_SET(ikey, cursor->key);
	/*
	 * We appended a padding byte to the key to avoid rewriting the last
	 * column.  Strip that away here.
	 */
	WT_ASSERT(session, ikey.size > 0);
	--ikey.size;

	ret = __curjoin_entry_in_range(session, cextract->entry, &ikey, false);
	if (ret == WT_NOTFOUND)
		ret = 0;
	else if (ret == 0)
		cextract->ismember = true;

	return (ret);
}

/*
 * __curjoin_entry_member --
 *	Do a membership check for a particular index that was joined,
 *	if not a member, returns WT_NOTFOUND.
 */
static int
__curjoin_entry_member(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin,
    WT_CURSOR_JOIN_ENTRY *entry, bool skip_left)
{
	WT_CURJOIN_EXTRACTOR extract_cursor;
	WT_CURSOR *c;
	WT_CURSOR_STATIC_INIT(iface,
	    __wt_cursor_get_key,		/* get-key */
	    __wt_cursor_get_value,		/* get-value */
	    __wt_cursor_set_key,		/* set-key */
	    __wt_cursor_set_value,		/* set-value */
	    __wt_cursor_compare_notsup,		/* compare */
	    __wt_cursor_equals_notsup,		/* equals */
	    __wt_cursor_notsup,			/* next */
	    __wt_cursor_notsup,			/* prev */
	    __wt_cursor_notsup,			/* reset */
	    __wt_cursor_notsup,			/* search */
	    __wt_cursor_search_near_notsup,	/* search-near */
	    __curjoin_extract_insert,		/* insert */
	    __wt_cursor_notsup,			/* update */
	    __wt_cursor_notsup,			/* remove */
	    __wt_cursor_reconfigure_notsup,	/* reconfigure */
	    __wt_cursor_notsup);		/* close */
	WT_DECL_RET;
	WT_INDEX *idx;
	WT_ITEM *key, v;
	bool bloom_found;

	if (skip_left && entry->ends_next == 1)
		return (0);	/* no checks to make */
	key = cjoin->iter->curkey;
	entry->stats.accesses++;
	bloom_found = false;

	if (entry->bloom != NULL) {
		/*
		 * If we don't own the Bloom filter, we must be sharing one
		 * in a previous entry. So the shared filter has already
		 * been checked and passed.
		 */
		if (!F_ISSET(entry, WT_CURJOIN_ENTRY_OWN_BLOOM))
			return (0);

		/*
		 * If the item is not in the Bloom filter, we return
		 * immediately, otherwise, we still need to check the
		 * long way.
		 */
		WT_ERR(__wt_bloom_inmem_get(entry->bloom, key));
		bloom_found = true;
	}
	if (entry->index != NULL) {
		/*
		 * If this entry is used by the iterator, then we already
		 * have the index key, and we won't have to do any extraction
		 * either.
		 */
		if (entry == cjoin->iter->entry)
			WT_ITEM_SET(v, cjoin->iter->idxkey);
		else {
			memset(&v, 0, sizeof(v));  /* Keep lint quiet. */
			c = entry->main;
			c->set_key(c, key);
			if ((ret = c->search(c)) == 0)
				ret = c->get_value(c, &v);
			else if (ret == WT_NOTFOUND)
				WT_ERR_MSG(session, WT_ERROR,
				    "main table for join is missing entry");
			WT_TRET(c->reset(c));
			WT_ERR(ret);
		}
	} else
		WT_ITEM_SET(v, *key);

	if ((idx = entry->index) != NULL && idx->extractor != NULL &&
	    entry != cjoin->iter->entry) {
		WT_CLEAR(extract_cursor);
		extract_cursor.iface = iface;
		extract_cursor.iface.session = &session->iface;
		extract_cursor.iface.key_format = idx->exkey_format;
		extract_cursor.ismember = false;
		extract_cursor.entry = entry;
		WT_ERR(idx->extractor->extract(idx->extractor,
		    &session->iface, key, &v, &extract_cursor.iface));
		if (!extract_cursor.ismember)
			WT_ERR(WT_NOTFOUND);
	} else
		WT_ERR(__curjoin_entry_in_range(session, entry, &v, skip_left));

	if (0) {
err:		if (ret == WT_NOTFOUND && bloom_found)
			entry->stats.bloom_false_positive++;
	}
	return (ret);
}

/*
 * __curjoin_next --
 *	WT_CURSOR::next for join cursors.
 */
static int
__curjoin_next(WT_CURSOR *cursor)
{
	WT_CURSOR *c;
	WT_CURSOR_JOIN *cjoin;
	WT_CURSOR_JOIN_ITER *iter;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	bool skip_left;
	u_int i;

	cjoin = (WT_CURSOR_JOIN *)cursor;

	CURSOR_API_CALL(cursor, session, next, NULL);

	if (F_ISSET(cjoin, WT_CURJOIN_ERROR))
		WT_ERR_MSG(session, WT_ERROR,
		    "join cursor encountered previous error");
	if (!F_ISSET(cjoin, WT_CURJOIN_INITIALIZED))
		WT_ERR(__curjoin_init_iter(session, cjoin));

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	iter = cjoin->iter;

nextkey:
	if ((ret = __curjoin_entry_iter_next(iter, cursor)) == 0) {
		F_SET(cursor, WT_CURSTD_KEY_EXT);

		/*
		 * We may have already established membership for the
		 * 'left' case for the first entry, since we're
		 * using that in our iteration.
		 */
		skip_left = F_ISSET(cjoin, WT_CURJOIN_SKIP_FIRST_LEFT);
		for (i = 0; i < cjoin->entries_next; i++) {
			ret = __curjoin_entry_member(session, cjoin,
			    &cjoin->entries[i], skip_left);
			if (ret == WT_NOTFOUND) {
				/*
				 * If this is compare=eq on our outer iterator,
				 * and we've moved past it, we're done.
				 */
				if (iter->isequal && i == 0)
					break;
				goto nextkey;
			}
			skip_left = false;
			WT_ERR(ret);
		}
	} else if (ret != WT_NOTFOUND)
		WT_ERR(ret);

	if (ret == 0) {
		/*
		 * Position the 'main' cursor, this will be used to
		 * retrieve values from the cursor join.
		 */
		c = iter->main;
		c->set_key(c, iter->curkey);
		if ((ret = c->search(c)) != 0)
			WT_ERR(c->search(c));
		F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
	}

	if (0) {
err:		F_SET(cjoin, WT_CURJOIN_ERROR);
	}
	API_END_RET(session, ret);
}

/*
 * __curjoin_reset --
 *	WT_CURSOR::reset for join cursors.
 */
static int
__curjoin_reset(WT_CURSOR *cursor)
{
	WT_CURSOR_JOIN *cjoin;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cjoin = (WT_CURSOR_JOIN *)cursor;

	CURSOR_API_CALL(cursor, session, reset, NULL);

	if (F_ISSET(cjoin, WT_CURJOIN_INITIALIZED))
		WT_ERR(__curjoin_entry_iter_reset(cjoin->iter));

err:	API_END_RET(session, ret);
}

/*
 * __curjoin_close --
 *	WT_CURSOR::close for join cursors.
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

	CURSOR_API_CALL(cursor, session, close, NULL);

	__wt_schema_release_table(session, cjoin->table);
	/* These are owned by the table */
	cursor->internal_uri = NULL;
	cursor->key_format = NULL;
	if (cjoin->projection != NULL) {
		__wt_free(session, cjoin->projection);
		__wt_free(session, cursor->value_format);
	}

	for (entry = cjoin->entries, i = 0; i < cjoin->entries_next;
		entry++, i++) {
		if (entry->main != NULL)
			WT_TRET(entry->main->close(entry->main));
		if (F_ISSET(entry, WT_CURJOIN_ENTRY_OWN_BLOOM))
			WT_TRET(__wt_bloom_close(entry->bloom));
		for (end = &entry->ends[0];
		     end < &entry->ends[entry->ends_next]; end++) {
			F_CLR(end->cursor, WT_CURSTD_JOINED);
			if (F_ISSET(end, WT_CURJOIN_END_OWN_CURSOR))
				WT_TRET(end->cursor->close(end->cursor));
		}
		__wt_free(session, entry->ends);
		__wt_free(session, entry->repack_format);
	}

	if (cjoin->iter != NULL)
		WT_TRET(__curjoin_entry_iter_close(cjoin->iter));
	__wt_free(session, cjoin->entries);
	WT_TRET(__wt_cursor_close(cursor));

err:	API_END_RET(session, ret);
}

/*
 * __wt_curjoin_open --
 *	Initialize a join cursor.
 *
 *	Join cursors are read-only.
 */
int
__wt_curjoin_open(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    __curjoin_get_key,			/* get-key */
	    __curjoin_get_value,		/* get-value */
	    __wt_cursor_set_key_notsup,		/* set-key */
	    __wt_cursor_set_value_notsup,	/* set-value */
	    __wt_cursor_compare_notsup,		/* compare */
	    __wt_cursor_equals_notsup,		/* equals */
	    __curjoin_next,			/* next */
	    __wt_cursor_notsup,			/* prev */
	    __curjoin_reset,			/* reset */
	    __wt_cursor_notsup,			/* search */
	    __wt_cursor_search_near_notsup,	/* search-near */
	    __wt_cursor_notsup,			/* insert */
	    __wt_cursor_notsup,			/* update */
	    __wt_cursor_notsup,			/* remove */
	    __wt_cursor_reconfigure_notsup,	/* reconfigure */
	    __curjoin_close);			/* close */
	WT_CURSOR *cursor;
	WT_CURSOR_JOIN *cjoin;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_TABLE *table;
	size_t size;
	const char *tablename, *columns;

	WT_STATIC_ASSERT(offsetof(WT_CURSOR_JOIN, iface) == 0);

	if (!WT_PREFIX_SKIP(uri, "join:"))
		return (EINVAL);
	tablename = uri;
	if (!WT_PREFIX_SKIP(tablename, "table:"))
		return (EINVAL);

	columns = strchr(tablename, '(');
	if (columns == NULL)
		size = strlen(tablename);
	else
		size = WT_PTRDIFF(columns, tablename);
	WT_RET(__wt_schema_get_table(session, tablename, size, 0, &table));

	WT_RET(__wt_calloc_one(session, &cjoin));
	cursor = &cjoin->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->internal_uri = table->name;
	cursor->key_format = table->key_format;
	cursor->value_format = table->value_format;
	cjoin->table = table;

	/* Handle projections. */
	WT_ERR(__wt_scr_alloc(session, 0, &tmp));
	if (columns != NULL) {
		WT_ERR(__wt_struct_reformat(session, table,
		    columns, strlen(columns), NULL, 1, tmp));
		WT_ERR(__wt_strndup(
		    session, tmp->data, tmp->size, &cursor->value_format));
		WT_ERR(__wt_strdup(session, columns, &cjoin->projection));
	}

	if (owner != NULL)
		WT_ERR(EINVAL);

	WT_ERR(__wt_cursor_init(cursor, uri, owner, cfg, cursorp));

	if (0) {
err:		WT_TRET(__curjoin_close(cursor));
		*cursorp = NULL;
	}

	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __wt_curjoin_join --
 *	Add a new join to a join cursor.
 */
int
__wt_curjoin_join(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin,
    WT_INDEX *idx, WT_CURSOR *ref_cursor, uint8_t flags, uint8_t range,
    uint64_t count, uint32_t bloom_bit_count, uint32_t bloom_hash_count)
{
	WT_CURSOR_INDEX *cindex;
	WT_CURSOR_JOIN_ENDPOINT *end;
	WT_CURSOR_JOIN_ENTRY *entry;
	WT_DECL_RET;
	bool hasins, needbloom, range_eq;
	char *main_uri, *newformat;
	const char *raw_cfg[] = { WT_CONFIG_BASE(
	    session, WT_SESSION_open_cursor), "raw", NULL };
	size_t len, newsize;
	u_int i, ins, nonbloom;

	entry = NULL;
	hasins = needbloom = false;
	ins = 0; /* -Wuninitialized */
	main_uri = NULL;
	nonbloom = 0; /* -Wuninitialized */

	for (i = 0; i < cjoin->entries_next; i++) {
		if (cjoin->entries[i].index == idx) {
			entry = &cjoin->entries[i];
			break;
		}
		if (!needbloom && i > 0 &&
		    !F_ISSET(&cjoin->entries[i], WT_CURJOIN_ENTRY_BLOOM)) {
			needbloom = true;
			nonbloom = i;
		}
	}
	if (entry == NULL) {
		WT_ERR(__wt_realloc_def(session, &cjoin->entries_allocated,
		    cjoin->entries_next + 1, &cjoin->entries));
		if (LF_ISSET(WT_CURJOIN_ENTRY_BLOOM) && needbloom) {
			/*
			 * Reorder the list so that after the first entry,
			 * the Bloom filtered entries come next, followed by
			 * the non-Bloom entries.  Once the Bloom filters
			 * are built, determining membership via Bloom is
			 * faster than without Bloom, so we can answer
			 * membership questions more quickly, and with less
			 * I/O, with the Bloom entries first.
			 */
			entry = &cjoin->entries[nonbloom];
			memmove(entry + 1, entry,
			    (cjoin->entries_next - nonbloom) *
			    sizeof(WT_CURSOR_JOIN_ENTRY));
			memset(entry, 0, sizeof(WT_CURSOR_JOIN_ENTRY));
		}
		else
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
			WT_ERR_MSG(session, EINVAL,
			    "count=%" PRIu64 " does not match "
			    "previous count=%" PRIu64 " for this index",
			    count, entry->count);
		if (LF_MASK(WT_CURJOIN_ENTRY_BLOOM) !=
		    F_MASK(entry, WT_CURJOIN_ENTRY_BLOOM))
			WT_ERR_MSG(session, EINVAL,
			    "join has incompatible strategy "
			    "values for the same index");

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
			if ((F_ISSET(end, WT_CURJOIN_END_GT) &&
			    ((range & WT_CURJOIN_END_GT) != 0 || range_eq)) ||
			    (F_ISSET(end, WT_CURJOIN_END_LT) &&
			    ((range & WT_CURJOIN_END_LT) != 0 || range_eq)) ||
			    (WT_CURJOIN_END_RANGE(end) == WT_CURJOIN_END_EQ &&
			    (range & (WT_CURJOIN_END_LT | WT_CURJOIN_END_GT))
			    != 0))
				WT_ERR_MSG(session, EINVAL,
				    "join has overlapping ranges");
			if (range == WT_CURJOIN_END_EQ &&
			    WT_CURJOIN_END_RANGE(end) == WT_CURJOIN_END_EQ &&
			    !F_ISSET(entry, WT_CURJOIN_ENTRY_DISJUNCTION))
				WT_ERR_MSG(session, EINVAL,
				    "compare=eq can only be combined "
				    "using operation=or");

			/*
			 * Sort "gt"/"ge" to the front, followed by any number
			 * of "eq", and finally "lt"/"le".
			 */
			if (!hasins &&
			    ((range & WT_CURJOIN_END_GT) != 0 ||
			    (range == WT_CURJOIN_END_EQ &&
			    !F_ISSET(end, WT_CURJOIN_END_GT)))) {
				ins = i;
				hasins = true;
			}
		}
		/* All checks completed, merge any new configuration now */
		entry->count = count;
		entry->bloom_bit_count =
		    WT_MAX(entry->bloom_bit_count, bloom_bit_count);
		entry->bloom_hash_count =
		    WT_MAX(entry->bloom_hash_count, bloom_hash_count);
	}
	WT_ERR(__curjoin_insert_endpoint(session, entry,
	    hasins ? ins : entry->ends_next, &end));
	end->cursor = ref_cursor;
	F_SET(end, range);

	/* Open the main file with a projection of the indexed columns. */
	if (entry->main == NULL && idx != NULL) {
		newsize = strlen(cjoin->table->name) + idx->colconf.len + 1;
		WT_ERR(__wt_calloc(session, 1, newsize, &main_uri));
		snprintf(main_uri, newsize, "%s%.*s",
		    cjoin->table->name, (int)idx->colconf.len,
		    idx->colconf.str);
		WT_ERR(__wt_open_cursor(session, main_uri,
		    (WT_CURSOR *)cjoin, raw_cfg, &entry->main));
		if (idx->extractor == NULL) {
			/*
			 * Add no-op padding so trailing 'u' formats are not
			 * transformed to 'U'.  This matches what happens in
			 * the index.  We don't do this when we have an
			 * extractor, extractors already use the padding
			 * byte trick.
			 */
			len = strlen(entry->main->value_format) + 3;
			WT_ERR(__wt_calloc(session, len, 1, &newformat));
			snprintf(newformat, len, "%s0x",
			    entry->main->value_format);
			__wt_free(session, entry->main->value_format);
			entry->main->value_format = newformat;
		}

		/*
		 * When we are repacking index keys to remove the primary
		 * key, we never want to transform trailing 'u'.  Use no-op
		 * padding to force this.
		 */
		cindex = (WT_CURSOR_INDEX *)ref_cursor;
		len = strlen(cindex->iface.key_format) + 3;
		WT_ERR(__wt_calloc(session, len, 1, &entry->repack_format));
		snprintf(entry->repack_format, len, "%s0x",
		    cindex->iface.key_format);
	}

err:	__wt_free(session, main_uri);
	return (ret);
}

/*
 * __curjoin_insert_endpoint --
 *	Insert a new entry into the endpoint array for the join entry.
 */
static int
__curjoin_insert_endpoint(WT_SESSION_IMPL *session, WT_CURSOR_JOIN_ENTRY *entry,
    u_int pos, WT_CURSOR_JOIN_ENDPOINT **newendp)
{
	WT_CURSOR_JOIN_ENDPOINT *newend;

	WT_RET(__wt_realloc_def(session, &entry->ends_allocated,
	    entry->ends_next + 1, &entry->ends));
	newend = &entry->ends[pos];
	memmove(newend + 1, newend,
	    (entry->ends_next - pos) * sizeof(WT_CURSOR_JOIN_ENDPOINT));
	memset(newend, 0, sizeof(WT_CURSOR_JOIN_ENDPOINT));
	entry->ends_next++;
	*newendp = newend;

	return (0);
}
