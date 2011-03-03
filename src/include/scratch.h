/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

typedef struct {
	WT_ITEM item;
	void *mem;
	uint32_t mem_size;

	uint32_t flags;
} WT_BUF;
