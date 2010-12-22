/* Copyright (c) 2010 WiredTiger, Inc.  All rights reserved. */

struct WT_CURSOR_STD;	typedef struct WT_CURSOR_STD WT_CURSOR_STD;

struct WT_CURSOR_STD {
	WT_CURSOR interface;

	WT_ITEM key;
	size_t keybufsz;
	WT_ITEM value;
	size_t valuebufsz;
};
