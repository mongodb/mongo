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
__wt_key_set(void *ref, const void *key, uint32_t size)
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
 * __wt_page_write_gen_check --
 *	Confirm the page's write generation number is correct.
 */
static inline int
__wt_page_write_gen_check(WT_PAGE *page, uint32_t write_gen)
{
	return (page->write_gen == write_gen ? 0 : WT_RESTART);
}

/*
 * __wt_key_cell_next --
 *	Move to the next key WT_CELL on the page (helper function for the
 *	WT_ROW_AND_KEY_FOREACH macro).
 */
static inline WT_CELL *
__wt_key_cell_next(WT_CELL *key_cell)
{
	/*
	 * Row-store leaf pages may have a single data cell between each key, or
	 * keys may be adjacent (when the data cell is empty).  Move to the next
	 * key.
	 */
	key_cell = WT_CELL_NEXT(key_cell);
	if (WT_CELL_TYPE(key_cell) != WT_CELL_KEY &&
	    WT_CELL_TYPE(key_cell) != WT_CELL_KEY_OVFL)
		key_cell = WT_CELL_NEXT(key_cell);
	return (key_cell);
}

/*
 * __wt_ref_off_page --
 *	Return if a pointer references off-page data.
 */
static inline int
__wt_ref_off_page(WT_PAGE *page, const void *p)
{
	/*
	 * There may be no underlying page, in which case the reference is
	 * off-page by definition.
	 */
	if (page->XXdsk == NULL)
		return (1);

	return (p < (void *)page->XXdsk ||
	    p >= (void *)((uint8_t *)page->XXdsk + page->size) ? 1 : 0);
}
