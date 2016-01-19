/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

static inline int __wt_log_cmp(WT_LSN *lsn1, WT_LSN *lsn2);

/*
 * __wt_log_cmp --
 *	Compare 2 LSNs, return -1 if lsn1 < lsn2, 0if lsn1 == lsn2
 *	and 1 if lsn1 > lsn2.
 */
static inline int
__wt_log_cmp(WT_LSN *lsn1, WT_LSN *lsn2)
{
	WT_LSN l1, l2;

	/*
	 * Read LSNs into local variables so that we only read each field
	 * once and all comparisons are on the same values.
	 */
	l1 = *(volatile WT_LSN *)lsn1;
	l2 = *(volatile WT_LSN *)lsn2;

	/*
	 * If the file numbers are different we don't need to compare the
	 * offset.
	 */
	if (l1.file != l2.file)
		return (l1.file < l2.file ? -1 : 1);
	/*
	 * If the file numbers are the same, compare the offset.
	 */
	if (l1.offset != l2.offset)
	    return (l1.offset < l2.offset ? -1 : 1);
	return (0);
}
