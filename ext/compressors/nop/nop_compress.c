/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>

#include <wiredtiger.h>
/*! [WT_EXTENSION_API declaration] */
#include <wiredtiger_ext.h>

static WT_EXTENSION_API *wt_api;
/*! [WT_EXTENSION_API declaration] */

static int
nop_compress(WT_COMPRESSOR *, WT_SESSION *,
    uint8_t *, size_t, uint8_t *, size_t, size_t *, int *);
static int
nop_decompress(WT_COMPRESSOR *, WT_SESSION *,
    uint8_t *, size_t, uint8_t *, size_t, size_t *);

static WT_COMPRESSOR nop_compressor = {
    nop_compress, NULL, nop_decompress, NULL };

/*! [WT_EXTENSION_API initialization] */
int
wiredtiger_extension_init(
    WT_SESSION *session, WT_EXTENSION_API *api, const char *config)
{
	WT_CONNECTION *conn;

	(void)config;					/* Unused */

	wt_api = api;
	conn = session->connection;

	return (conn->add_compressor(conn, "nop", &nop_compressor, NULL));
}
/*! [WT_EXTENSION_API initialization] */

/* Implementation of WT_COMPRESSOR for WT_CONNECTION::add_compressor. */
static int
nop_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	(void)compressor;				/* Unused */
	(void)session;

	*compression_failed = 0;
	if (dst_len < src_len) {
		*compression_failed = 1;
		return (0);
	}

	memcpy(dst, src, src_len);
	*result_lenp = src_len;

	return (0);
}

static int
nop_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	(void)compressor;				/* Unused */
	(void)session;
	(void)src_len;

	/*
	 * The destination length is the number of uncompressed bytes we're
	 * expected to return.
	 */
	memcpy(dst, src, dst_len);
	*result_lenp = dst_len;
	return (0);
}
/* End implementation of WT_COMPRESSOR. */
