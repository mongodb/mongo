/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/* Logging subsystem declarations. */
typedef enum {
	WT_LOGREC_INT16,
	WT_LOGREC_UINT16,
	WT_LOGREC_INT32,
	WT_LOGREC_UINT32,
	WT_LOGREC_INT64,
	WT_LOGREC_UINT64,
	WT_LOGREC_STRING,
} WT_LOGREC_FIELDTYPE;

typedef struct {
	const char *fmt;
	const char *fields[];
} WT_LOGREC_DESC;
