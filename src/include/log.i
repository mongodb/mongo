/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
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
	volatile WT_LSN l1, l2;

	/*
	 * Read LSNs into local variables so that we only read each field
	 * once and all comparisons are on the same values.
	 */
	l1 = *lsn1;
	l2 = *lsn2;

	return ((l1.file != l2.file ?
	    (l1.file < l2.file ? -1 : 1) :
	    (l1.offset != l2.offset ?
	    (l1.offset < l2.offset ? -1 : 1) : 0)));
}
