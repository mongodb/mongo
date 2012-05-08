/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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

#include <bzlib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

WT_EXTENSION_API *wt_api;

static int
bzip2_compress(WT_COMPRESSOR *,
    WT_SESSION *, uint8_t *, size_t, uint8_t *, size_t, size_t *, int *);
static int
bzip2_decompress(WT_COMPRESSOR *,
    WT_SESSION *, uint8_t *, size_t, uint8_t *, size_t, size_t *);

static WT_COMPRESSOR bzip2_compressor = {
    bzip2_compress, bzip2_decompress, NULL };

#define	__UNUSED(v)	((void)(v))

/* between 0-4: set the amount of verbosity to stderr */
static int bz_verbosity = 0;

/* between 1-9: set the block size to 100k x this number (compression only) */
static int bz_blocksize100k = 1;

/*
 * between 0-250: workFactor: see bzip2 manual.  0 is a reasonable default
 * (compression only)
 */
static int bz_workfactor = 0;

/* if nonzero, decompress using less memory, but slower (decompression only) */
static int bz_small = 0;

int
wiredtiger_extension_init(
    WT_SESSION *session, WT_EXTENSION_API *api, const char *config)
{
	WT_CONNECTION *conn;

	__UNUSED(config);

	wt_api = api;
	conn = session->connection;

	return (conn->add_compressor(
	    conn, "bzip2_compress", &bzip2_compressor, NULL));
}

/* Bzip2 WT_COMPRESSOR implementation for WT_CONNECTION::add_compressor. */
/*
 * bzip2_error --
 *	Output an error message, and return a standard error code.
 */
static int
bzip2_error(WT_SESSION *session, const char *call, int bzret)
{
	const char *msg;

	switch (bzret) {
	case BZ_MEM_ERROR:
		msg = "BZ_MEM_ERROR";
		break;
	case BZ_OUTBUFF_FULL:
		msg = "BZ_OUTBUFF_FULL";
		break;
	case BZ_SEQUENCE_ERROR:
		msg = "BZ_SEQUENCE_ERROR";
		break;
	case BZ_PARAM_ERROR:
		msg = "BZ_PARAM_ERROR";
		break;
	case BZ_DATA_ERROR:
		msg = "BZ_DATA_ERROR";
		break;
	case BZ_DATA_ERROR_MAGIC:
		msg = "BZ_DATA_ERROR_MAGIC";
		break;
	case BZ_IO_ERROR:
		msg = "BZ_IO_ERROR";
		break;
	case BZ_UNEXPECTED_EOF:
		msg = "BZ_UNEXPECTED_EOF";
		break;
	case BZ_CONFIG_ERROR:
		msg = "BZ_CONFIG_ERROR";
		break;
	default:
		msg = "unknown error";
		break;
	}

	(void)wiredtiger_err_printf(
	    session, "bzip2 error: %s: %s: %d", call, msg, bzret);
	return (WT_ERROR);
}

static void *
bzalloc(void *cookie, int number, int size)
{
	return (wiredtiger_scr_alloc(cookie, (size_t)number * size));
}

static void
bzfree(void *cookie, void *p)
{
	wiredtiger_scr_free(cookie, p);
}

static int
bzip2_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	bz_stream bz;
	int ret;

	__UNUSED(compressor);

	memset(&bz, 0, sizeof(bz));
	bz.bzalloc = bzalloc;
	bz.bzfree = bzfree;
	bz.opaque = session;

	if ((ret = BZ2_bzCompressInit(&bz,
	    bz_blocksize100k, bz_verbosity, bz_workfactor)) != BZ_OK)
		return (bzip2_error(session, "BZ2_bzCompressInit", ret));

	bz.next_in = (char *)src;
	bz.avail_in = (uint32_t)src_len;
	bz.next_out = (char *)dst;
	bz.avail_out = (uint32_t)dst_len;
	if ((ret = BZ2_bzCompress(&bz, BZ_FINISH)) == BZ_STREAM_END) {
		*compression_failed = 0;
		*result_lenp = dst_len - bz.avail_out;
	} else
		*compression_failed = 1;

	if ((ret = BZ2_bzCompressEnd(&bz)) != BZ_OK)
		return (bzip2_error(session, "BZ2_bzCompressEnd", ret));

	return (0);
}

static int
bzip2_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	bz_stream bz;
	int ret, tret;

	__UNUSED(compressor);

	memset(&bz, 0, sizeof(bz));
	bz.bzalloc = bzalloc;
	bz.bzfree = bzfree;
	bz.opaque = session;

	if ((ret = BZ2_bzDecompressInit(&bz, bz_small, bz_verbosity)) != BZ_OK)
		return (bzip2_error(session, "BZ2_bzDecompressInit", ret));

	bz.next_in = (char *)src;
	bz.avail_in = (uint32_t)src_len;
	bz.next_out = (char *)dst;
	bz.avail_out = (uint32_t)dst_len;
	if ((ret = BZ2_bzDecompress(&bz)) == BZ_STREAM_END) {
		*result_lenp = dst_len - bz.avail_out;
		ret = 0;
	} else
		bzip2_error(session, "BZ2_bzDecompress", ret);

	if ((tret = BZ2_bzDecompressEnd(&bz)) != BZ_OK)
		return (bzip2_error(session, "BZ2_bzDecompressEnd", tret));

	return (ret == 0 ?
	    0 : bzip2_error(session, "BZ2_bzDecompressEnd", ret));
}
/* End Bzip2 WT_COMPRESSOR implementation for WT_CONNECTION::add_compressor. */
