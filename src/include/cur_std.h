/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct WT_CURSOR_STD;	typedef struct WT_CURSOR_STD WT_CURSOR_STD;

struct WT_CURSOR_STD {
	WT_CURSOR interface;

	WT_DATAITEM key;
	void *keybuf;
	size_t keybufsz;
	WT_DATAITEM value;
	void *valuebuf;
	size_t valuebufsz;

#define	WT_CURSTD_BADKEY	0x1U
#define	WT_CURSTD_BADVALUE	0x2U
#define	WT_CURSTD_RAW		0x4U
	uint32_t flags;
};
