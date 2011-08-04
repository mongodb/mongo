/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/* The fixed name of the schema file. */
#define	WT_SCHEMA_FILENAME	"__schema.wt"

/* Character constants for projection plans. */
#define	WT_PROJ_KEY	'k' /* Go to key in cursor <arg>. */
#define	WT_PROJ_NEXT	'n' /* Process the next item (<arg> repeats). */
#define	WT_PROJ_REUSE	'r' /* Reuse the previous item (<arg> repeats). */
#define	WT_PROJ_SKIP	's' /* Skip a column in the cursor (<arg> repeats). */
#define	WT_PROJ_VALUE	'v' /* Go to the value in cursor <arg>. */
