/* DO NOT EDIT: automatically built by dist/stat.py. */

#include "wt_internal.h"

int
__wt_stat_alloc_fh(IENV *ienv, WT_STATS **statsp)
{
	WT_STATS *stats;
	int ret;

	if ((ret = __wt_calloc(ienv,
	    WT_STAT_FH_TOTAL_ENTRIES + 1, sizeof(WT_STATS), &stats)) != 0)
		return (ret);

	stats[WT_STAT_READ_IO].desc = "count of read I/Os";
	stats[WT_STAT_WRITE_IO].desc = "count of write I/Os";

	*statsp = stats;
	return (0);
}

int
__wt_stat_clear_fh(WT_STATS *stats)
{
	stats[WT_STAT_READ_IO].v = 0;
	stats[WT_STAT_WRITE_IO].v = 0;
	return (0);
}

int
__wt_stat_alloc_db(IENV *ienv, WT_STATS **statsp)
{
	WT_STATS *stats;
	int ret;

	if ((ret = __wt_calloc(ienv,
	    WT_STAT_DB_TOTAL_ENTRIES + 1, sizeof(WT_STATS), &stats)) != 0)
		return (ret);

	stats[WT_STAT_BULK_PAIRS_READ].desc = "bulk key/data pairs inserted";
	stats[WT_STAT_BULK_DUP_DATA_READ].desc =
	    "bulk duplicate data pairs read";
	stats[WT_STAT_BULK_OVERFLOW_KEY].desc = "bulk overflow key items read";
	stats[WT_STAT_BULK_OVERFLOW_DATA].desc =
	    "bulk overflow data items read";

	*statsp = stats;
	return (0);
}

int
__wt_stat_clear_db(WT_STATS *stats)
{
	stats[WT_STAT_BULK_PAIRS_READ].v = 0;
	stats[WT_STAT_BULK_DUP_DATA_READ].v = 0;
	stats[WT_STAT_BULK_OVERFLOW_KEY].v = 0;
	stats[WT_STAT_BULK_OVERFLOW_DATA].v = 0;
	return (0);
}

int
__wt_stat_alloc_env(IENV *ienv, WT_STATS **statsp)
{
	WT_STATS *stats;
	int ret;

	if ((ret = __wt_calloc(ienv,
	    WT_STAT_ENV_TOTAL_ENTRIES + 1, sizeof(WT_STATS), &stats)) != 0)
		return (ret);

	stats[WT_STAT_CACHE_ALLOC].desc = "pages allocated in the cache";
	stats[WT_STAT_CACHE_HIT].desc = "reads found in the cache";
	stats[WT_STAT_CACHE_MISS].desc = "reads not found in the cache";
	stats[WT_STAT_CACHE_DIRTY].desc = "dirty pages in the cache";
	stats[WT_STAT_CACHE_WRITE_EVICT].desc =
	    "dirty pages evicted from the cache";
	stats[WT_STAT_CACHE_EVICT].desc = "clean pages evicted from the cache";
	stats[WT_STAT_CACHE_WRITE].desc = "writes from the cache";

	*statsp = stats;
	return (0);
}

int
__wt_stat_clear_env(WT_STATS *stats)
{
	stats[WT_STAT_CACHE_ALLOC].v = 0;
	stats[WT_STAT_CACHE_HIT].v = 0;
	stats[WT_STAT_CACHE_MISS].v = 0;
	stats[WT_STAT_CACHE_DIRTY].v = 0;
	stats[WT_STAT_CACHE_WRITE_EVICT].v = 0;
	stats[WT_STAT_CACHE_EVICT].v = 0;
	stats[WT_STAT_CACHE_WRITE].v = 0;
	return (0);
}
