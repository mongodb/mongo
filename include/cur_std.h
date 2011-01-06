/* Copyright (c) 2010 WiredTiger, Inc.  All rights reserved. */

struct WT_CURSOR_STD;	typedef struct WT_CURSOR_STD WT_CURSOR_STD;

struct WT_CURSOR_STD {
	WT_CURSOR interface;

	WT_ITEM key;
	void *keybuf;
	size_t keybufsz;
	WT_ITEM value;
	void *valuebuf;
	size_t valuebufsz;

#define	WT_CURSTD_BADKEY	0x1
#define	WT_CURSTD_BADVALUE	0x2
#define	WT_CURSTD_RAW		0x4
	uint32_t flags;
};
