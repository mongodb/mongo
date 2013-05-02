/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_LOG_FILENAME	"WiredTiger.log"		/* Log file name */

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

typedef struct {
	uint32_t	fileid;
} WT_LOG;
