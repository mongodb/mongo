/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * __wt_cache_pages_inuse --
 *	Return the number of pages in use.
 */
static inline uint64_t
__wt_cache_pages_inuse(WT_CACHE *cache)
{
	uint64_t pages_in, pages_out;

	/*
	 * Reading 64-bit fields, potentially on 32-bit machines, and other
	 * threads of control may be modifying them.  Check them for sanity
	 * (although "interesting" corruption is vanishingly unlikely, these
	 * values just increment over time).
	 */
	pages_in = cache->stat_pages_in;
	pages_out = cache->stat_pages_out;
	return (pages_in > pages_out ? pages_in - pages_out : 0);
}

/*
 * __wt_cache_bytes_inuse --
 *	Return the number of bytes in use.
 */
static inline uint64_t
__wt_cache_bytes_inuse(WT_CACHE *cache)
{
	uint64_t bytes_in, bytes_out;

	/*
	 * Reading 64-bit fields, potentially on 32-bit machines, and other
	 * threads of control may be modifying them.  Check them for sanity
	 * (although "interesting" corruption is vanishingly unlikely, these
	 * values just increment over time).
	 */
	bytes_in = cache->stat_bytes_in;
	bytes_out = cache->stat_bytes_out;
	return (bytes_in > bytes_out ? bytes_in - bytes_out : 0);
}

/*
 * __wt_key_set --
 *	Set a key/size pair, where the key does not require further processing.
 */
static inline void
__wt_key_set(void *ref, void *key, uint32_t size)
{
	/*
	 * Passed both WT_ROW_REF and WT_ROW structures; the first two fields
	 * of the structures are a void *data/uint32_t size pair.
	 */
	((WT_ROW *)ref)->key = key;
	((WT_ROW *)ref)->size = size;
}

/*
 * __wt_key_set_process --
 *	Set a key/size pair, where the key requires further processing.
 */
static inline void
__wt_key_set_process(void *ref, void *key)
{
	/*
	 * Passed both WT_ROW_REF and WT_ROW structures; the first two fields
	 * of the structures are a void *data/uint32_t size pair.
	 */
	((WT_ROW *)ref)->key = key;
	((WT_ROW *)ref)->size = 0;
}

/*
 * __wt_key_process --
 *	Return if a key requires processing.
 */
static inline int
__wt_key_process(void *ref)
{
	/*
	 * Passed both WT_ROW_REF and WT_ROW structures; the first two fields
	 * of the structures are a void *data/uint32_t size pair.
	 */
	return (((WT_ROW *)ref)->size == 0 ? 1 : 0);
}

/*
 * __wt_page_write --
 *	Write a file page.
 */
static inline int
__wt_page_write(WT_TOC *toc, WT_PAGE *page)
{
	return (__wt_disk_write(toc, page->dsk, page->addr, page->size));
}

/*
 * __wt_set_ff_and_sa_from_offset --
 *	Set first-free and space-available values from an address positioned
 *	one past the last used byte on the page.
 */
static inline void
__wt_set_ff_and_sa_from_offset(WT_PAGE *page,
    void *p, uint8_t **first_freep, uint32_t *space_availp)
{
	*first_freep = (uint8_t *)p;
	*space_availp =
	    page->size - (uint32_t)((uint8_t *)p - (uint8_t *)page->dsk);
}

/*
 * __wt_page_write_gen_check --
 *	Confirm the page's write generation number is correct.
 */
static inline int
__wt_page_write_gen_check(WT_PAGE *page, uint32_t write_gen)
{
	return (page->write_gen == write_gen ? 0 : WT_RESTART);
}

/*
 * __wt_key_item_next --
 *	Move to the next key WT_ITEM on the page (helper function for the
 *	WT_ROW_AND_KEY_FOREACH macro).
 */
static inline WT_ITEM *
__wt_key_item_next(WT_ITEM *key_item)
{
	/*
	 * Row-store leaf pages may have a single data item between each key, or
	 * keys may be adjacent (when the data item is empty).  Move to the next
	 * key.
	 */
	key_item = WT_ITEM_NEXT(key_item);
	if (WT_ITEM_TYPE(key_item) != WT_ITEM_KEY &&
	    WT_ITEM_TYPE(key_item) != WT_ITEM_KEY_OVFL)
		key_item = WT_ITEM_NEXT(key_item);
	return (key_item);
}
