/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_desc_read --
 *	Read and verify the file's metadata.
 */
int
__wt_desc_read(SESSION *session)
{
	BTREE *btree;
	WT_BTREE_DESC *desc;
	WT_CONFIG_ITEM cval;
	uint8_t *p, *ep, buf[512];

	/*
	 * We currently always do the verification step, because it's cheap
	 * and we only do it the first time a file is opened.
	 */
	btree = session->btree;

	/* Read the first sector. */
	WT_RET(__wt_read(session, btree->fh, (off_t)0, sizeof(buf), buf));

	/*
	 * Verify there's a string, and that it's nul-terminated.
	 *
	 * XXX
	 * Should we do basic checks here, only printable characters?
	 *
	 * XXX
	 * Should we call a schema-layer validate function to prove it's
	 * correctly formed?
	 *
	 * XXX
	 * For verification we should compare the configuration string with
	 * whatever is the backup configuration string, wherever it happens
	 * to be?
	 */
	for (p = buf + sizeof(WT_BTREE_DESC), ep = buf + sizeof(buf);; ++p) {
		if (p == ep) {
			__wt_errx(session,
			    "no configuration string found in the file");
			return (WT_ERROR);
		}
		if (*p == '\0')
			break;
	}

	/* Copy the file's configuration string. */
	WT_RET(__wt_strdup(
	    session, (char *)(buf + sizeof(WT_BTREE_DESC)), &btree->config));

	/* Get the allocation size, we need it to validate the addresses. */
	WT_RET(__wt_config_getones(btree->config, "allocation_size", &cval));
	btree->allocsize = (uint32_t)cval.val;

	desc = (WT_BTREE_DESC *)buf;
	if (desc->root_addr != WT_ADDR_INVALID &&
	    WT_ADDR_TO_OFF(btree, desc->root_addr) +
	    desc->root_size > btree->fh->file_size)
		goto eof;
	btree->root_page.addr = desc->root_addr;
	btree->root_page.size = desc->root_size;
	if (desc->free_addr != WT_ADDR_INVALID &&
	    WT_ADDR_TO_OFF(btree, desc->free_addr) +
	    desc->free_size > btree->fh->file_size)
		goto eof;
	btree->free_addr = desc->free_addr;
	btree->free_size = desc->free_size;
	if (desc->config_addr != WT_ADDR_INVALID &&
	    WT_ADDR_TO_OFF(btree, desc->config_addr) +
	    desc->config_size > btree->fh->file_size) {
eof:		__wt_errx(session,
		    "file root, free or configuration addresses reference "
		    "non-existent file pages");
		return (WT_ERROR);
	}

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
