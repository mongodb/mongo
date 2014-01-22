/*-
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

#include <zlib.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

static int
zlib_compress(WT_COMPRESSOR *, WT_SESSION *,
    uint8_t *, size_t, uint8_t *, size_t, size_t *, int *);
static int
zlib_decompress(WT_COMPRESSOR *, WT_SESSION *,
    uint8_t *, size_t, uint8_t *, size_t, size_t *);
static int
zlib_terminate(WT_COMPRESSOR *, WT_SESSION *);
static int
zlib_compress_raw(WT_COMPRESSOR *, WT_SESSION *, size_t, int,
    size_t, uint8_t *, uint32_t *, uint32_t, uint8_t *, size_t, int,
    size_t *, uint32_t *);

/* Local compressor structure. */
typedef struct {
	WT_COMPRESSOR compressor;		/* Must come first */

	WT_EXTENSION_API *wt_api;		/* Extension API */

	int zlib_level;			/* Configuration */
} ZLIB_COMPRESSOR;

/*
 * Bzip gives us a cookie to pass to the underlying allocation functions; we
 * we need two handles, package them up.
 */
typedef struct {
	WT_COMPRESSOR *compressor;
	WT_SESSION *session;
} ZLIB_OPAQUE;

int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	ZLIB_COMPRESSOR *zlib_compressor;

	(void)config;				/* Unused parameters */

	if ((zlib_compressor = calloc(1, sizeof(ZLIB_COMPRESSOR))) == NULL)
		return (errno);

	zlib_compressor->compressor.compress = zlib_compress;
#if 1
	zlib_compressor->compressor.compress_raw = zlib_compress_raw;
#else
	(void)zlib_compress_raw;
	zlib_compressor->compressor.compress_raw = NULL;
#endif
	zlib_compressor->compressor.decompress = zlib_decompress;
	zlib_compressor->compressor.pre_size = NULL;
	zlib_compressor->compressor.terminate = zlib_terminate;

	zlib_compressor->wt_api = connection->get_extension_api(connection);

	/*
	 * between 0-10: level: see zlib manual.
	 */
	zlib_compressor->zlib_level = Z_DEFAULT_COMPRESSION;

						/* Load the compressor */
	return (connection->add_compressor(
	    connection, "zlib", (WT_COMPRESSOR *)zlib_compressor, NULL));
}

/*
 * zlib_error --
 *	Output an error message, and return a standard error code.
 */
static int
zlib_error(
    WT_COMPRESSOR *compressor, WT_SESSION *session, const char *call, int zret)
{
	WT_EXTENSION_API *wt_api;

	wt_api = ((ZLIB_COMPRESSOR *)compressor)->wt_api;

	(void)wt_api->err_printf(wt_api, session,
	    "zlib error: %s: %s: %d", call, zError(zret), zret);
	return (WT_ERROR);
}

static void *
zalloc(void *cookie, u_int number, u_int size)
{
	ZLIB_OPAQUE *opaque;
	WT_EXTENSION_API *wt_api;

	opaque = cookie;
	wt_api = ((ZLIB_COMPRESSOR *)opaque->compressor)->wt_api;
	return (wt_api->scr_alloc(
	    wt_api, opaque->session, (size_t)(number * size)));
}

static void
zfree(void *cookie, void *p)
{
	ZLIB_OPAQUE *opaque;
	WT_EXTENSION_API *wt_api;

	opaque = cookie;
	wt_api = ((ZLIB_COMPRESSOR *)opaque->compressor)->wt_api;
	wt_api->scr_free(wt_api, opaque->session, p);
}

static int
zlib_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	ZLIB_COMPRESSOR *zlib_compressor;
	ZLIB_OPAQUE opaque;
	z_stream zs;
	size_t result_len;
	int ret;

	zlib_compressor = (ZLIB_COMPRESSOR *)compressor;

	memset(&zs, 0, sizeof(zs));
	zs.zalloc = zalloc;
	zs.zfree = zfree;
	opaque.compressor = compressor;
	opaque.session = session;
	zs.opaque = &opaque;

	if ((ret = deflateInit(&zs, zlib_compressor->zlib_level)) != Z_OK)
		return (zlib_error(
		    compressor, session, "deflateInit", ret));

	zs.next_in = src;
	zs.avail_in = (uint32_t)src_len;
	zs.next_out = dst;
	zs.avail_out = (uint32_t)dst_len - 1;
	if ((ret = deflate(&zs, Z_FINISH)) == Z_STREAM_END) {
		*compression_failed = 0;
		result_len = zs.total_out;
		/* Append 1: there is a single block to decompress. */
#if 0
		dst[result_len++] = 1;
#endif
		*result_lenp = result_len;
	} else
		*compression_failed = 1;

	if ((ret = deflateEnd(&zs)) != Z_OK)
		return (
		    zlib_error(compressor, session, "deflateEnd", ret));

	return (0);
}

static inline uint32_t
zlib_find_slot(uint32_t target, uint32_t *offsets, uint32_t slots)
{
	uint32_t base, indx, limit;

	/* Figure out which slot we got to: binary search */
	if (target < offsets[1])
		indx = 0;
	else if (target >= offsets[slots])
		indx = slots;
	else
		for (base = 1, limit = slots - 1; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);

			if (target < offsets[indx])
				continue;
			base = indx + 1;
			--limit;
		}

	return (indx);
}

/*
 * zlib_compress_raw --
 *	Test function for the test/format utility.
 */
static int
zlib_compress_raw(WT_COMPRESSOR *compressor, WT_SESSION *session,
    size_t page_max, int split_pct, size_t extra,
    uint8_t *src, uint32_t *offsets, uint32_t slots,
    uint8_t *dst, size_t dst_len, int final,
    size_t *result_lenp, uint32_t *result_slotsp)
{
	ZLIB_COMPRESSOR *zlib_compressor;
	ZLIB_OPAQUE opaque;
	z_stream last_zs, zs;
	uint32_t curr_slot, last_slot;
	int ret;

	curr_slot = last_slot = 0;
	(void)split_pct;
	(void)dst_len;
	(void)final;

	zlib_compressor = (ZLIB_COMPRESSOR *)compressor;

	memset(&zs, 0, sizeof(zs));
	zs.zalloc = zalloc;
	zs.zfree = zfree;
	opaque.compressor = compressor;
	opaque.session = session;
	zs.opaque = &opaque;

	if ((ret = deflateInit(&zs,
	    zlib_compressor->zlib_level)) != Z_OK)
		return (zlib_error(
		    compressor, session, "deflateInit", ret));

	zs.next_in = src;
	zs.next_out = dst;
#define	WT_ZLIB_RESERVED	6
	zs.avail_out = (uint32_t)(page_max - extra - WT_ZLIB_RESERVED);
	last_zs = zs;

	/*
	 * Strategy: take the available output size and compress that much
	 * input.  Continue until there is no input small enough or the
	 * compression fails to fit.
	 */
	for (;;) {
		/* Find the slot we will try to compress up to. */
		if ((curr_slot = zlib_find_slot(
		    zs.total_in + zs.avail_out, offsets, slots)) == last_slot)
			break;

		zs.avail_in = offsets[curr_slot] - offsets[last_slot];
		/* Save the stream state in case the chosen data doesn't fit. */
		last_zs = zs;

		if ((ret = deflate(&zs, Z_SYNC_FLUSH)) != Z_OK)
			return (
			    zlib_error(compressor, session, "deflate", ret));

		if (zs.avail_out == 0 && zs.avail_in > 0) {
			/* Roll back the last operation: it didn't complete */
			zs = last_zs;
			break;
		}

		last_slot = curr_slot;
	}

	zs.avail_out += WT_ZLIB_RESERVED;
	if ((ret = deflate(&zs, Z_FINISH)) != Z_STREAM_END)
		return (
		    zlib_error(compressor, session, "deflate", ret));

	if (last_slot > 0) {
		*result_slotsp = last_slot;
		*result_lenp = zs.total_out;
	}

	if ((ret = deflateEnd(&zs)) != Z_OK)
		return (
		    zlib_error(compressor, session, "deflateEnd", ret));

#if 0
	fprintf(stderr,
	    "zlib_compress_raw (%s): page_max %" PRIuMAX ", slots %" PRIu32
	    ", take %" PRIu32 ": %" PRIu32 " -> %" PRIuMAX "\n",
	    final ? "final" : "not final", (uintmax_t)page_max,
	    slots, last_slot, offset[last_slot], (uintmax_t)*result_lenp);
#endif
	return (0);
}

static int
zlib_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	ZLIB_OPAQUE opaque;
	z_stream zs;
	int ret, tret;

	memset(&zs, 0, sizeof(zs));
	zs.zalloc = zalloc;
	zs.zfree = zfree;
	opaque.compressor = compressor;
	opaque.session = session;
	zs.opaque = &opaque;

	if ((ret = inflateInit(&zs)) != Z_OK)
		return (zlib_error(
		    compressor, session, "inflateInit", ret));

	zs.next_in = src;
	zs.avail_in = (uint32_t)src_len;
	zs.next_out = dst;
	zs.avail_out = (uint32_t)dst_len;
	if ((ret = inflate(&zs, Z_FINISH)) == Z_STREAM_END) {
		*result_lenp = dst_len - zs.avail_out;
		ret = Z_OK;
	}

	if ((tret = inflateEnd(&zs)) != Z_OK && ret == Z_OK)
		ret = tret;

	return (ret == Z_OK ?
	    0 : zlib_error(compressor, session, "inflate", ret));
}

static int
zlib_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
	(void)session;					/* Unused parameters */

	free(compressor);
	return (0);
}
