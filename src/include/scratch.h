/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct WT_SCRATCH;	typedef struct WT_SCRATCH WT_SCRATCH;

struct WT_SCRATCH {
	WT_DATAITEM item;
	void *buf;
	uint32_t mem_size;

	uint32_t flags;
};
