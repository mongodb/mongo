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
__wt_read(IENV *ienv,
    WT_FH *fh, u_int32_t block_number, u_int32_t blocks, void *buf)
{
	ENV *env;
	ssize_t bytes_to_read, bytes_read;
	int ret;

	env = ienv->env;

	WT_STAT(fh->read_count);
	if (FLD_ISSET(env->verbose, WT_VERB_FILEOPS_ALL))
		__wt_env_errx(env,
		    "fileops: %s: read %lu 512B blocks at block %lu",
		    fh->name, (u_long)blocks, (u_long)block_number);
	
	bytes_to_read = (ssize_t)WT_BLOCKS_TO_BYTES(blocks);
	bytes_read = pread(fh->fd, buf,
	    (ssize_t)WT_BLOCKS_TO_BYTES(blocks),
	    (off_t)WT_BLOCKS_TO_BYTES(block_number));
	if (bytes_to_read == bytes_read)
		return (0);

	__wt_env_err(env, errno, "Read error; file %s", fh->name);
	return (WT_ERROR);
}

/*
 * __wt_write --
 *	Write a file handle.
 */
int
__wt_write(IENV *ienv,
    WT_FH *fh, u_int32_t block_number, u_int32_t blocks, void *buf)
{
	ENV *env;
	ssize_t bytes_to_write, bytes_written;
	int ret;

	env = ienv->env;
	
	WT_STAT(fh->write_count);
	if (FLD_ISSET(env->verbose, WT_VERB_FILEOPS_ALL))
		__wt_env_errx(env,
		    "fileops: %s: write %lu 512B blocks at block %lu",
		    fh->name, (u_long)blocks, (u_long)block_number);

	bytes_to_write = (ssize_t)WT_BLOCKS_TO_BYTES(blocks);
	bytes_written = pwrite(fh->fd, buf,
	    (ssize_t)WT_BLOCKS_TO_BYTES(blocks),
	    (off_t)WT_BLOCKS_TO_BYTES(block_number));
	if (bytes_to_write == bytes_written)
		return (0);

	__wt_env_err(env, errno, "Write error; file %s", fh->name);
	return (WT_ERROR);
}
