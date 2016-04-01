/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_stream_set_line_buffer --
 *	Set line buffering on a stream.
 */
void
__wt_stream_set_line_buffer(FILE *fp)
{
	/*
	 * This function exists because MSVC doesn't support buffer sizes of 0
	 * to the setvbuf call. To avoid re-introducing the bug, we have helper
	 * functions and disallow calling setvbuf directly in WiredTiger code.
	 */
	(void)setvbuf(fp, NULL, _IOLBF, 1024);
}

/*
 * __wt_stream_set_no_buffer --
 *	Turn off buffering on a stream.
 */
void
__wt_stream_set_no_buffer(FILE *fp)
{
	(void)setvbuf(fp, NULL, _IONBF, 0);
}
