/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_read --
 *	Read a file handle.
 */
int
__wt_read(IENV *ienv, WT_FH *fh, u_int32_t block_number,
    u_int32_t blocks, void *buf, ssize_t *bytes_read_ret)
{
	ssize_t bytes_to_read, bytes_read;
	int ret;

	WT_STAT(fh->read_count);
	
	bytes_to_read = (ssize_t)WT_BLOCKS_TO_BYTES(blocks);
	bytes_read = pread(fh->fd, buf,
	    (ssize_t)WT_BLOCKS_TO_BYTES(blocks),
	    (ssize_t)WT_BLOCKS_TO_BYTES(block_number));
	if (bytes_to_read == bytes_read) {
		*bytes_read_ret = bytes_read;
		return (0);
	}
	*bytes_read_ret = 0;
	return (WT_ERROR);
}

/*
 * __wt_write --
 *	Write a file handle.
 */
int
__wt_write(IENV *ienv, WT_FH *fh, u_int32_t block_number,
    u_int32_t blocks, void *buf, ssize_t *bytes_written_ret)
{
	ssize_t bytes_to_write, bytes_written;
	int ret;
	
	WT_STAT(fh->write_count);
	bytes_to_write = (ssize_t)WT_BLOCKS_TO_BYTES(blocks);
	bytes_written = pwrite(fh->fd, buf,
	    (ssize_t)WT_BLOCKS_TO_BYTES(blocks),
	    (ssize_t)WT_BLOCKS_TO_BYTES(block_number));
	if (bytes_to_write == bytes_written) {
		*bytes_written_ret = bytes_written;
		return (0);
	}
	*bytes_written_ret = 0;
	return (WT_ERROR);
}
