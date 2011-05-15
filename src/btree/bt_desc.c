/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_desc_read --
 *	Read the file's descriptor structure.
 */
int
__wt_desc_read(SESSION *session)
{
	BTREE *btree;
	WT_BTREE_DESC *desc;
	uint8_t buf[512];

	btree = session->btree;

	/* Read the first sector. */
	WT_RET(__wt_read(session, btree->fh, (off_t)0, sizeof(buf), buf));

	/* Copy the file's configuration string. */
	WT_RET(__wt_strdup(
	    session, (char *)(buf + sizeof(WT_BTREE_DESC)), &btree->config));

	desc = (WT_BTREE_DESC *)buf;
	btree->root_page.addr = desc->root_addr;
	btree->root_page.size = desc->root_size;
	btree->free_addr = desc->free_addr;
	btree->free_size = desc->free_size;
	btree->lsn = desc->lsn;

	return (0);
}

/*
 * __wt_desc_write --
 *	Write the file's descriptor structure.
 */
int
__wt_desc_write(SESSION *session, const char *config, WT_FH *fh)
{
	WT_BTREE_DESC *desc;
	uint8_t buf[512];

	memset(buf, 0, sizeof(buf));
	desc = (WT_BTREE_DESC *)buf;

	desc->magic = WT_BTREE_MAGIC;
	desc->majorv = WT_BTREE_MAJOR_VERSION;
	desc->minorv = WT_BTREE_MINOR_VERSION;

	desc->root_addr = WT_ADDR_INVALID;
	desc->free_addr = WT_ADDR_INVALID;
	desc->config_addr = WT_ADDR_INVALID;

	strcpy((char *)(buf + sizeof(WT_BTREE_DESC)), config);

	return (__wt_write(session, fh, (off_t)0, 512, buf));
}

/*
 * __wt_desc_update --
 *	Update the file's descriptor structure.
 */
int
__wt_desc_update(SESSION *session)
{
	BTREE *btree;
	WT_BTREE_DESC *desc;
	uint8_t buf[512];

	btree = session->btree;

	/* Read the first sector. */
	WT_RET(__wt_read(session, btree->fh, (off_t)0, sizeof(buf), buf));
	desc = (WT_BTREE_DESC *)buf;

	/* Update the possibly changed fields. */
	if (desc->root_addr == btree->root_page.addr &&
	    desc->free_addr == btree->free_addr)
		return (0);

	desc->root_addr = btree->root_page.addr;
	desc->root_size = btree->root_page.size;
	desc->free_addr = btree->free_addr;
	desc->free_size = btree->free_size;
	desc->lsn = btree->lsn;

	/* Write the first sector. */
	return (__wt_write(session, btree->fh, (off_t)0, 512, buf));
}
