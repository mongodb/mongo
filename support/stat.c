/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

int
__wt_stat_alloc_fh_stats(ENV *env, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(env,
	    WT_STAT_FH_STATS_TOTAL + 1, sizeof(WT_STATS), &stats));

	stats[WT_STAT_READ_IO].desc = "read I/Os";
	stats[WT_STAT_WRITE_IO].desc = "write I/Os";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_fh_stats(WT_STATS *stats)
{
	stats[WT_STAT_READ_IO].v = 0;
	stats[WT_STAT_WRITE_IO].v = 0;
}

int
__wt_stat_alloc_idb_dstats(ENV *env, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(env,
	    WT_STAT_IDB_DSTATS_TOTAL + 1, sizeof(WT_STATS), &stats));

	stats[WT_STAT_BASE_RECNO].desc = "base record number";
	stats[WT_STAT_EXTSIZE].desc = "database extent size";
	stats[WT_STAT_FRAGSIZE].desc = "database fragment size";
	stats[WT_STAT_INTLSIZE].desc = "internal page size";
	stats[WT_STAT_ITEM_DATA_OVFL].desc = "overflow data items";
	stats[WT_STAT_ITEM_DUP_DATA].desc = "duplicate data items";
	stats[WT_STAT_ITEM_KEY_OVFL].desc = "overflow keys";
	stats[WT_STAT_ITEM_TOTAL_DATA].desc = "total database data items";
	stats[WT_STAT_ITEM_TOTAL_KEY].desc = "total database keys";
	stats[WT_STAT_LEAFSIZE].desc = "leaf page size";
	stats[WT_STAT_MAGIC].desc = "magic number";
	stats[WT_STAT_MAJOR].desc = "major version number";
	stats[WT_STAT_MINOR].desc = "minor version number";
	stats[WT_STAT_PAGE_DUP_INTERNAL].desc = "duplicate internal pages";
	stats[WT_STAT_PAGE_DUP_LEAF].desc = "duplicate leaf pages";
	stats[WT_STAT_PAGE_FREE].desc = "unused on-page space in bytes";
	stats[WT_STAT_PAGE_INTERNAL].desc = "primary internal pages";
	stats[WT_STAT_PAGE_LEAF].desc = "primary leaf pages";
	stats[WT_STAT_PAGE_OVERFLOW].desc = "overflow pages";
	stats[WT_STAT_TREE_LEVEL].desc = "number of levels in the Btree";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_idb_dstats(WT_STATS *stats)
{
	stats[WT_STAT_BASE_RECNO].v = 0;
	stats[WT_STAT_EXTSIZE].v = 0;
	stats[WT_STAT_FRAGSIZE].v = 0;
	stats[WT_STAT_INTLSIZE].v = 0;
	stats[WT_STAT_ITEM_DATA_OVFL].v = 0;
	stats[WT_STAT_ITEM_DUP_DATA].v = 0;
	stats[WT_STAT_ITEM_KEY_OVFL].v = 0;
	stats[WT_STAT_ITEM_TOTAL_DATA].v = 0;
	stats[WT_STAT_ITEM_TOTAL_KEY].v = 0;
	stats[WT_STAT_LEAFSIZE].v = 0;
	stats[WT_STAT_MAGIC].v = 0;
	stats[WT_STAT_MAJOR].v = 0;
	stats[WT_STAT_MINOR].v = 0;
	stats[WT_STAT_PAGE_DUP_INTERNAL].v = 0;
	stats[WT_STAT_PAGE_DUP_LEAF].v = 0;
	stats[WT_STAT_PAGE_FREE].v = 0;
	stats[WT_STAT_PAGE_INTERNAL].v = 0;
	stats[WT_STAT_PAGE_LEAF].v = 0;
	stats[WT_STAT_PAGE_OVERFLOW].v = 0;
	stats[WT_STAT_TREE_LEVEL].v = 0;
}

int
__wt_stat_alloc_idb_stats(ENV *env, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(env,
	    WT_STAT_IDB_STATS_TOTAL + 1, sizeof(WT_STATS), &stats));

	stats[WT_STAT_BULK_DUP_DATA_READ].desc =
	    "bulk duplicate data pairs read";
	stats[WT_STAT_BULK_OVERFLOW_DATA].desc =
	    "bulk overflow data items read";
	stats[WT_STAT_BULK_OVERFLOW_KEY].desc = "bulk overflow key items read";
	stats[WT_STAT_BULK_PAIRS_READ].desc = "bulk key/data pairs inserted";
	stats[WT_STAT_DB_CACHE_ALLOC].desc = "pages allocated in the cache";
	stats[WT_STAT_DB_CACHE_HIT].desc =
	    "cache hit: reads found in the cache";
	stats[WT_STAT_DB_CACHE_MISS].desc =
	    "cache miss: reads not found in the cache";
	stats[WT_STAT_DB_READ_BY_KEY].desc = "database read-by-key operations";
	stats[WT_STAT_DB_READ_BY_RECNO].desc =
	    "database read-by-recno operations";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_idb_stats(WT_STATS *stats)
{
	stats[WT_STAT_BULK_DUP_DATA_READ].v = 0;
	stats[WT_STAT_BULK_OVERFLOW_DATA].v = 0;
	stats[WT_STAT_BULK_OVERFLOW_KEY].v = 0;
	stats[WT_STAT_BULK_PAIRS_READ].v = 0;
	stats[WT_STAT_DB_CACHE_ALLOC].v = 0;
	stats[WT_STAT_DB_CACHE_HIT].v = 0;
	stats[WT_STAT_DB_CACHE_MISS].v = 0;
	stats[WT_STAT_DB_READ_BY_KEY].v = 0;
	stats[WT_STAT_DB_READ_BY_RECNO].v = 0;
}

int
__wt_stat_alloc_ienv_stats(ENV *env, WT_STATS **statsp)
{
	WT_STATS *stats;

	WT_RET(__wt_calloc(env,
	    WT_STAT_IENV_STATS_TOTAL + 1, sizeof(WT_STATS), &stats));

	stats[WT_STAT_CACHE_ALLOC].desc = "pages allocated in the cache";
	stats[WT_STAT_CACHE_CLEAN].desc = "clean pages in the cache";
	stats[WT_STAT_CACHE_DIRTY].desc = "dirty pages in the cache";
	stats[WT_STAT_CACHE_EVICT].desc = "clean pages evicted from the cache";
	stats[WT_STAT_CACHE_HIT].desc = "cache hit: reads found in the cache";
	stats[WT_STAT_CACHE_MISS].desc =
	    "cache miss: reads not found in the cache";
	stats[WT_STAT_CACHE_WRITE].desc = "writes from the cache";
	stats[WT_STAT_CACHE_WRITE_EVICT].desc =
	    "dirty pages evicted from the cache";
	stats[WT_STAT_DATABASE_OPEN].desc = "database open";
	stats[WT_STAT_HASH_BUCKETS].desc = "hash buckets";
	stats[WT_STAT_LONGEST_BUCKET].desc =
	    "longest hash bucket chain search";
	stats[WT_STAT_MEMALLOC].desc = "memory allocations";
	stats[WT_STAT_MEMFREE].desc = "memory frees";
	stats[WT_STAT_MTX_LOCK].desc = "mutex lock calls";
	stats[WT_STAT_TOTAL_READ_IO].desc = "total read I/Os";
	stats[WT_STAT_TOTAL_WRITE_IO].desc = "total write I/Os";
	stats[WT_STAT_WORKQ_CACHE_ALLOC_REQUESTS].desc =
	    "workQ cache allocations";
	stats[WT_STAT_WORKQ_PASSES].desc = "workQ queue passes";
	stats[WT_STAT_WORKQ_REQUESTS].desc = "workQ requests";
	stats[WT_STAT_WORKQ_SLEEP].desc = "workQ sleeps";
	stats[WT_STAT_WORKQ_YIELD].desc = "workQ yields";

	*statsp = stats;
	return (0);
}

void
__wt_stat_clear_ienv_stats(WT_STATS *stats)
{
	stats[WT_STAT_CACHE_ALLOC].v = 0;
	stats[WT_STAT_CACHE_CLEAN].v = 0;
	stats[WT_STAT_CACHE_DIRTY].v = 0;
	stats[WT_STAT_CACHE_EVICT].v = 0;
	stats[WT_STAT_CACHE_HIT].v = 0;
	stats[WT_STAT_CACHE_MISS].v = 0;
	stats[WT_STAT_CACHE_WRITE].v = 0;
	stats[WT_STAT_CACHE_WRITE_EVICT].v = 0;
	stats[WT_STAT_DATABASE_OPEN].v = 0;
	stats[WT_STAT_HASH_BUCKETS].v = 0;
	stats[WT_STAT_LONGEST_BUCKET].v = 0;
	stats[WT_STAT_MEMALLOC].v = 0;
	stats[WT_STAT_MEMFREE].v = 0;
	stats[WT_STAT_MTX_LOCK].v = 0;
	stats[WT_STAT_TOTAL_READ_IO].v = 0;
	stats[WT_STAT_TOTAL_WRITE_IO].v = 0;
	stats[WT_STAT_WORKQ_CACHE_ALLOC_REQUESTS].v = 0;
	stats[WT_STAT_WORKQ_PASSES].v = 0;
	stats[WT_STAT_WORKQ_REQUESTS].v = 0;
	stats[WT_STAT_WORKQ_SLEEP].v = 0;
	stats[WT_STAT_WORKQ_YIELD].v = 0;
}
