/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_block_buffer_to_addr --
 *	Convert a filesystem address cookie into its components.
 */
int
__wt_block_buffer_to_addr(
    const uint8_t *p, uint32_t *addr, uint32_t *size, uint32_t *cksum)
{
	uint64_t a;

	WT_RET(__wt_vunpack_uint(&p, 0, &a));
	if (addr != NULL)
		*addr = (uint32_t)a;

	WT_RET(__wt_vunpack_uint(&p, 0, &a));
	if (size != NULL)
		*size = (uint32_t)a;

	if (cksum != NULL) {
		WT_RET(__wt_vunpack_uint(&p, 0, &a));
		*cksum = (uint32_t)a;
	}

	return (0);
}

/*
 * __wt_block_addr_to_buffer --
 *	Convert the filesystem components into its address cookie.
 */
int
__wt_block_addr_to_buffer(
    uint8_t **p, uint32_t addr, uint32_t size, uint32_t cksum)
{
	uint64_t a;

	a = addr;
	WT_RET(__wt_vpack_uint(p, 0, a));
	a = size;
	WT_RET(__wt_vpack_uint(p, 0, a));
	a = cksum;
	WT_RET(__wt_vpack_uint(p, 0, a));
	return (0);
}
