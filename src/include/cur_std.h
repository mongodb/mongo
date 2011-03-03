/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct WT_CURSOR_STD;	typedef struct WT_CURSOR_STD WT_CURSOR_STD;

struct WT_CURSOR_STD {
	WT_CURSOR iface;

	TAILQ_ENTRY(WT_CURSOR_STD) q;		/* List of open cursors. */

	WT_BUF key, value;

#define	WT_CURSTD_BADKEY	0x01
#define	WT_CURSTD_BADVALUE	0x02
#define	WT_CURSTD_DUMPKEY	0x04
#define	WT_CURSTD_DUMPVALUE	0x08
#define	WT_CURSTD_POSITIONED	0x10
#define	WT_CURSTD_RAW		0x20
	uint32_t flags;
};
