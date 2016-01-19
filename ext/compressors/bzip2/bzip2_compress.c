/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
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
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

/* Local compressor structure. */
typedef struct {
	WT_COMPRESSOR compressor;		/* Must come first */

	WT_EXTENSION_API *wt_api;		/* Extension API */

	int bz_verbosity;			/* Configuration */
	int bz_blocksize100k;
	int bz_workfactor;
	int bz_small;
} BZIP_COMPRESSOR;

/*
 * Bzip gives us a cookie to pass to the underlying allocation functions; we
 * we need two handles, package them up.
 */
typedef struct {
	WT_COMPRESSOR *compressor;
	WT_SESSION *session;
} BZIP_OPAQUE;

/*
 * bzip2_error --
 *	Output an error message, and return a standard error code.
 */
static int
bzip2_error(
    WT_COMPRESSOR *compressor, WT_SESSION *session, const char *call, int bzret)
{
	WT_EXTENSION_API *wt_api;
	const char *msg;

	wt_api = ((BZIP_COMPRESSOR *)compressor)->wt_api;

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

	(void)wt_api->err_printf(wt_api, session,
	    "bzip2 error: %s: %s: %d", call, msg, bzret);
	return (WT_ERROR);
}

/*
 * bzalloc --
 *	Allocate scratch buffers.
 */
static void *
bzalloc(void *cookie, int number, int size)
{
	BZIP_OPAQUE *opaque;
	WT_EXTENSION_API *wt_api;

	opaque = cookie;
	wt_api = ((BZIP_COMPRESSOR *)opaque->compressor)->wt_api;
	return (wt_api->scr_alloc(
	    wt_api, opaque->session, (size_t)(number * size)));
}

/*
 * bzfree --
 *	Free scratch buffers.
 */
static void
bzfree(void *cookie, void *p)
{
	BZIP_OPAQUE *opaque;
	WT_EXTENSION_API *wt_api;

	opaque = cookie;
	wt_api = ((BZIP_COMPRESSOR *)opaque->compressor)->wt_api;
	wt_api->scr_free(wt_api, opaque->session, p);
}

/*
 * bzip2_compress --
 *	WiredTiger bzip2 compression.
 */
static int
bzip2_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	BZIP_COMPRESSOR *bzip_compressor;
	BZIP_OPAQUE opaque;
	bz_stream bz;
	int ret;

	bzip_compressor = (BZIP_COMPRESSOR *)compressor;

	memset(&bz, 0, sizeof(bz));
	bz.bzalloc = bzalloc;
	bz.bzfree = bzfree;
	opaque.compressor = compressor;
	opaque.session = session;
	bz.opaque = &opaque;

	if ((ret = BZ2_bzCompressInit(&bz,
	    bzip_compressor->bz_blocksize100k,
	    bzip_compressor->bz_verbosity,
	    bzip_compressor->bz_workfactor)) != BZ_OK)
		return (bzip2_error(
		    compressor, session, "BZ2_bzCompressInit", ret));

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
		return (
		    bzip2_error(compressor, session, "BZ2_bzCompressEnd", ret));

	return (0);
}

/*
 * __bzip2_compress_raw_random --
 *	Return a 32-bit pseudo-random number.
 *
 * This is an implementation of George Marsaglia's multiply-with-carry pseudo-
 * random number generator.  Computationally fast, with reasonable randomness
 * properties.
 */
static uint32_t
__bzip2_compress_raw_random(void)
{
	static uint32_t m_w = 521288629;
	static uint32_t m_z = 362436069;

	m_z = 36969 * (m_z & 65535) + (m_z >> 16);
	m_w = 18000 * (m_w & 65535) + (m_w >> 16);
	return (m_z << 16) + (m_w & 65535);
}

/*
 * bzip2_compress_raw --
 *	Test function for the test/format utility.
 */
static int
bzip2_compress_raw(WT_COMPRESSOR *compressor, WT_SESSION *session,
    size_t page_max, int split_pct, size_t extra,
    uint8_t *src, uint32_t *offsets, uint32_t slots,
    uint8_t *dst, size_t dst_len, int final,
    size_t *result_lenp, uint32_t *result_slotsp)
{
	uint32_t take, twenty_pct;
	int compression_failed, ret;

	(void)page_max;					/* Unused  parameters */
	(void)split_pct;
	(void)extra;
	(void)final;

	/*
	 * This function is used by the test/format utility to test the
	 * WT_COMPRESSOR::compress_raw functionality.
	 *
	 * I'm trying to mimic how a real application is likely to behave: if
	 * it's a small number of slots, we're not going to take them because
	 * they aren't worth compressing.  In all likelihood, that's going to
	 * be because the btree is wrapping up a page, but that's OK, that is
	 * going to happen a lot. In addition, add a 2% chance of not taking
	 * anything at all just because we don't want to take it.  Otherwise,
	 * select between 80 and 100% of the slots and compress them, stepping
	 * down by 5 slots at a time until something works.
	 */
	take = slots;
	if (take < 10 || __bzip2_compress_raw_random() % 100 < 2)
		take = 0;
	else {
		twenty_pct = (slots / 10) * 2;
		if (twenty_pct < slots)
			take -= __bzip2_compress_raw_random() % twenty_pct;

		for (;;) {
			if ((ret = bzip2_compress(compressor, session,
			    src, offsets[take],
			    dst, dst_len,
			    result_lenp, &compression_failed)) != 0)
				return (ret);
			if (!compression_failed)
				break;
			if (take < 10) {
				take = 0;
				break;
			}
			take -= 5;
		}
	}

	*result_slotsp = take;
	if (take == 0)
		*result_lenp = 0;

#if 0
	fprintf(stderr,
	    "bzip2_compress_raw (%s): page_max %" PRIuMAX
	    ", split_pct %u, extra %" PRIuMAX
	    ", slots %" PRIu32 ", take %" PRIu32 ": %" PRIu32 " -> %"
	    PRIuMAX "\n",
	    final ? "final" : "not final",
	    (uintmax_t)page_max, split_pct, (uintmax_t)extra,
	    slots, take, offsets[take], (uintmax_t)*result_lenp);
#endif
	return (take == 0 ? EAGAIN : 0);
}

/*
 * bzip2_decompress --
 *	WiredTiger bzip2 decompression.
 */
static int
bzip2_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	BZIP_COMPRESSOR *bzip_compressor;
	BZIP_OPAQUE opaque;
	bz_stream bz;
	int ret, tret;

	bzip_compressor = (BZIP_COMPRESSOR *)compressor;

	memset(&bz, 0, sizeof(bz));
	bz.bzalloc = bzalloc;
	bz.bzfree = bzfree;
	opaque.compressor = compressor;
	opaque.session = session;
	bz.opaque = &opaque;

	if ((ret = BZ2_bzDecompressInit(&bz,
	    bzip_compressor->bz_small, bzip_compressor->bz_verbosity)) != BZ_OK)
		return (bzip2_error(
		    compressor, session, "BZ2_bzDecompressInit", ret));

	bz.next_in = (char *)src;
	bz.avail_in = (uint32_t)src_len;
	bz.next_out = (char *)dst;
	bz.avail_out = (uint32_t)dst_len;
	if ((ret = BZ2_bzDecompress(&bz)) == BZ_STREAM_END) {
		*result_lenp = dst_len - bz.avail_out;
		ret = 0;
	} else {
		/*
		 * If BZ2_bzDecompress returns 0, it expects there to be more
		 * data available.  There isn't, so treat this as an error.
		 */
		if (ret == 0)
			ret = BZ_DATA_ERROR;
		(void)bzip2_error(compressor, session, "BZ2_bzDecompress", ret);
	}

	if ((tret = BZ2_bzDecompressEnd(&bz)) != BZ_OK)
		return (bzip2_error(
		    compressor, session, "BZ2_bzDecompressEnd", tret));

	return (ret == 0 ?
	    0 : bzip2_error(compressor, session, "BZ2_bzDecompressEnd", ret));
}

/*
 * bzip2_terminate --
 *	WiredTiger bzip2 compression termination.
 */
static int
bzip2_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
	(void)session;					/* Unused parameters */

	free(compressor);
	return (0);
}

/*
 * bzip2_add_compressor --
 *	Add a bzip2 compressor.
 */
static int
bzip2_add_compressor(WT_CONNECTION *connection, int raw, const char *name)
{
	BZIP_COMPRESSOR *bzip_compressor;

	/*
	 * There are two almost identical bzip2 compressors: one supporting raw
	 * compression (used by test/format to test raw compression), the other
	 * without raw compression, that might be useful for real applications.
	 */
	if ((bzip_compressor = calloc(1, sizeof(BZIP_COMPRESSOR))) == NULL)
		return (errno);

	bzip_compressor->compressor.compress = bzip2_compress;
	bzip_compressor->
	    compressor.compress_raw = raw ? bzip2_compress_raw : NULL;
	bzip_compressor->compressor.decompress = bzip2_decompress;
	bzip_compressor->compressor.pre_size = NULL;
	bzip_compressor->compressor.terminate = bzip2_terminate;

	bzip_compressor->wt_api = connection->get_extension_api(connection);

	/* between 0-4: set the amount of verbosity to stderr */
	bzip_compressor->bz_verbosity = 0;

	/*
	 * between 1-9: set the block size to 100k x this number (compression
	 * only)
	 */
	bzip_compressor->bz_blocksize100k = 1;

	/*
	 * between 0-250: workFactor: see bzip2 manual.  0 is a reasonable
	 * default (compression only)
	 */
	bzip_compressor->bz_workfactor = 0;

	/*
	 * if nonzero, decompress using less memory, but slower (decompression
	 * only)
	 */
	bzip_compressor->bz_small = 0;

	return (connection->add_compressor(	/* Load the compressor */
	    connection, name, (WT_COMPRESSOR *)bzip_compressor, NULL));
}

/*
 * wiredtiger_extension_init --
 *	WiredTiger bzip2 compression extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	int ret;

	(void)config;				/* Unused parameters */

	if ((ret = bzip2_add_compressor(connection, 0, "bzip2")) != 0)
		return (ret);
	if ((ret = bzip2_add_compressor(connection, 1, "bzip2-raw-test")) != 0)
		return (ret);
	return (0);
}
