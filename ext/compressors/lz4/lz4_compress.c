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

#include <lz4.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger_config.h>
#include <wiredtiger.h>
#include <wiredtiger_ext.h>

/* Local compressor structure. */
typedef struct {
	WT_COMPRESSOR compressor;		/* Must come first */

	WT_EXTENSION_API *wt_api;		/* Extension API */
} LZ4_COMPRESSOR;

/*
 * LZ4 decompression requires the exact compressed byte count returned by the
 * LZ4_compress and LZ4_compress_destSize functions. WiredTiger doesn't track
 * that value, store it in the destination buffer.
 *
 * Additionally, LZ4_compress_destSize may compress into the middle of a record,
 * and after decompression we return the length to the last record successfully
 * decompressed, not the number of bytes decompressed; store that value in the
 * destination buffer as well.
 *
 * Use fixed-size, 4B values (WiredTiger never writes buffers larger than 4GB).
 *
 * The unused field is available for a mode flag if one is needed in the future,
 * we guarantee it's 0.
 */
typedef struct {
	uint32_t compressed_len;	/* True compressed length */
	uint32_t uncompressed_len;	/* True uncompressed source length */
	uint32_t useful_len;		/* Decompression return value */
	uint32_t unused;		/* Guaranteed to be 0 */
} LZ4_PREFIX;

#ifdef WORDS_BIGENDIAN
/*
 * lz4_bswap32 --
 *	32-bit unsigned little-endian to/from big-endian value.
 */
static inline uint32_t
lz4_bswap32(uint32_t v)
{
	return (
	    ((v << 24) & 0xff000000) |
	    ((v <<  8) & 0x00ff0000) |
	    ((v >>  8) & 0x0000ff00) |
	    ((v >> 24) & 0x000000ff)
	);
}

/*
 * lz4_prefix_swap --
 *	The additional information is written in little-endian format, handle
 * the conversion.
 */
static inline void
lz4_prefix_swap(LZ4_PREFIX *prefix)
{
	prefix->compressed_len = lz4_bswap32(prefix->compressed_len);
	prefix->uncompressed_len = lz4_bswap32(prefix->uncompressed_len);
	prefix->useful_len = lz4_bswap32(prefix->useful_len);
	prefix->unused = lz4_bswap32(prefix->unused);
}
#endif

/*
 * lz4_error --
 *	Output an error message, and return a standard error code.
 */
static int
lz4_error(
    WT_COMPRESSOR *compressor, WT_SESSION *session, const char *call, int error)
{
	WT_EXTENSION_API *wt_api;

	wt_api = ((LZ4_COMPRESSOR *)compressor)->wt_api;

	(void)wt_api->err_printf(wt_api,
	    session, "lz4 error: %s: %d", call, error);
	return (WT_ERROR);
}

/*
 *  lz4_compress --
 *	WiredTiger LZ4 compression.
 */
static int
lz4_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	LZ4_PREFIX prefix;
	int lz4_len;

	(void)compressor;				/* Unused parameters */
	(void)session;
	(void)dst_len;

	/* Compress, starting after the prefix bytes. */
	lz4_len = LZ4_compress(
	   (const char *)src, (char *)dst + sizeof(LZ4_PREFIX), (int)src_len);

	/*
	 * If compression succeeded and the compressed length is smaller than
	 * the original size, return success.
	 */
	if (lz4_len != 0 && (size_t)lz4_len + sizeof(LZ4_PREFIX) < src_len) {
		prefix.compressed_len = (uint32_t)lz4_len;
		prefix.uncompressed_len = (uint32_t)src_len;
		prefix.useful_len = (uint32_t)src_len;
		prefix.unused = 0;
#ifdef WORDS_BIGENDIAN
		lz4_prefix_swap(&prefix);
#endif
		memcpy(dst, &prefix, sizeof(LZ4_PREFIX));

		*result_lenp = (size_t)lz4_len + sizeof(LZ4_PREFIX);
		*compression_failed = 0;
		return (0);
	}

	*compression_failed = 1;
	return (0);
}

/*
 * lz4_decompress --
 *	WiredTiger LZ4 decompression.
 */
static int
lz4_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	WT_EXTENSION_API *wt_api;
	LZ4_PREFIX prefix;
	int decoded;
	uint8_t *dst_tmp;

	(void)src_len;					/* Unused parameters */

	wt_api = ((LZ4_COMPRESSOR *)compressor)->wt_api;

	/*
	 * Retrieve the true length of the compressed block and source and the
	 * decompressed bytes to return from the start of the source buffer.
	 */
	memcpy(&prefix, src, sizeof(LZ4_PREFIX));
#ifdef WORDS_BIGENDIAN
	lz4_prefix_swap(&prefix);
#endif

	/*
	 * Decompress, starting after the prefix bytes. Use safe decompression:
	 * we rely on decompression to detect corruption.
	 *
	 * Two code paths, one with and one without a bounce buffer. When doing
	 * raw compression, we compress to a target size irrespective of row
	 * boundaries, and return to our caller a "useful" compression length
	 * based on the last complete row that was compressed. Our caller stores
	 * that length, not the length of bytes actually compressed by LZ4. In
	 * other words, our caller doesn't know how many bytes will result from
	 * decompression, likely hasn't provided us a large enough buffer, and
	 * we have to allocate a scratch buffer.
	 */
	if (dst_len < prefix.uncompressed_len) {
		if ((dst_tmp = wt_api->scr_alloc(
		   wt_api, session, (size_t)prefix.uncompressed_len)) == NULL)
			return (ENOMEM);

		decoded = LZ4_decompress_safe(
		    (const char *)src + sizeof(LZ4_PREFIX), (char *)dst_tmp,
		    (int)prefix.compressed_len, (int)prefix.uncompressed_len);

		if (decoded >= 0)
			memcpy(dst, dst_tmp, dst_len);
		wt_api->scr_free(wt_api, session, dst_tmp);
	} else
		decoded = LZ4_decompress_safe(
		    (const char *)src + sizeof(LZ4_PREFIX),
		    (char *)dst, (int)prefix.compressed_len, (int)dst_len);

	if (decoded >= 0) {
		*result_lenp = prefix.useful_len;
		return (0);
	}

	return (
	    lz4_error(compressor, session, "LZ4 decompress error", decoded));
}

/*
 * lz4_find_slot --
 *	Find the slot containing the target offset (binary search).
 */
static inline uint32_t
lz4_find_slot(int target_arg, uint32_t *offsets, uint32_t slots)
{
	uint32_t base, indx, limit, target;

	indx = 1;					/* -Wuninitialized */

	target = (uint32_t)target_arg;			/* Type conversion */

	/* Fast check if we consumed it all, it's a likely result. */
	if (target >= offsets[slots])
		return (slots);

	/*
	 * Figure out which slot we got to: binary search. Note the test of
	 * offset (slot + 1), that's (end-byte + 1) for slot.
	 */
	for (base = 0, limit = slots; limit != 0; limit >>= 1) {
		indx = base + (limit >> 1);
		if (target > offsets[indx + 1]) {
			base = indx + 1;
			--limit;
		}
	}

	return (indx);
}

/*
 * lz4_compress_raw --
 *	Pack records into a specified on-disk page size.
 */
static int
lz4_compress_raw(WT_COMPRESSOR *compressor, WT_SESSION *session,
    size_t page_max, int split_pct, size_t extra,
    uint8_t *src, uint32_t *offsets, uint32_t slots,
    uint8_t *dst, size_t dst_len, int final,
    size_t *result_lenp, uint32_t *result_slotsp)
{
	LZ4_PREFIX prefix;
	int lz4_len;
	uint32_t slot;
	int sourceSize, targetDestSize;

	(void)compressor;				/* Unused parameters */
	(void)session;
	(void)split_pct;
	(void)final;

	sourceSize = (int)offsets[slots];		/* Type conversion */
	targetDestSize =
	    (int)((dst_len < page_max ? dst_len : page_max) - extra);

	/* Compress, starting after the prefix bytes. */
	lz4_len = LZ4_compress_destSize((const char *)src,
	    (char *)dst + sizeof(LZ4_PREFIX), &sourceSize, targetDestSize);

	/*
	 * If compression succeeded and the compressed length is smaller than
	 * the original size, return success.
	 */
	if (lz4_len != 0) {
		/* Find the first slot we didn't compress. */
		slot = lz4_find_slot(sourceSize, offsets, slots);

		if ((size_t)lz4_len + sizeof(LZ4_PREFIX) < offsets[slot]) {
			prefix.compressed_len = (uint32_t)lz4_len;
			prefix.uncompressed_len = (uint32_t)sourceSize;
			prefix.useful_len = offsets[slot];
			prefix.unused = 0;
#ifdef WORDS_BIGENDIAN
			lz4_prefix_swap(&prefix);
#endif
			memcpy(dst, &prefix, sizeof(LZ4_PREFIX));

			*result_slotsp = slot;
			*result_lenp = (size_t)lz4_len + sizeof(LZ4_PREFIX);
			return (0);
		}
	}

	*result_slotsp = 0;
	*result_lenp = 1;
	return (0);
}

/*
 * lz4_pre_size --
 *	WiredTiger LZ4 destination buffer sizing for compression.
 */
static int
lz4_pre_size(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len, size_t *result_lenp)
{
	(void)compressor;				/* Unused parameters */
	(void)session;
	(void)src;

	/*
	 * In block mode, LZ4 can use more space than the input data size, use
	 * the library calculation of that overhead (plus our overhead) to be
	 * safe.
	 */
	*result_lenp = LZ4_COMPRESSBOUND(src_len) + sizeof(LZ4_PREFIX);
	return (0);
}

/*
 * lz4_terminate --
 *	WiredTiger LZ4 compression termination.
 */
static int
lz4_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
	(void)session;					/* Unused parameters */

	free(compressor);
	return (0);
}

/*
 * lz4_add_compressor --
 *	Add a LZ4 compressor.
 */
static int
lz_add_compressor(WT_CONNECTION *connection, int raw, const char *name)
{
	LZ4_COMPRESSOR *lz4_compressor;

	/*
	 * There are two almost identical LZ4 compressors: one using raw
	 * compression to target a specific block size, and one without.
	 */
	if ((lz4_compressor = calloc(1, sizeof(LZ4_COMPRESSOR))) == NULL)
		return (errno);

	lz4_compressor->compressor.compress = lz4_compress;
	lz4_compressor->compressor.compress_raw = raw ? lz4_compress_raw : NULL;
	lz4_compressor->compressor.decompress = lz4_decompress;
	lz4_compressor->compressor.pre_size = lz4_pre_size;
	lz4_compressor->compressor.terminate = lz4_terminate;

	lz4_compressor->wt_api = connection->get_extension_api(connection);

	/* Load the compressor */
	return (connection->add_compressor(
	    connection, name, (WT_COMPRESSOR *)lz4_compressor, NULL));
}

int lz4_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);

/*
 * lz4_extension_init --
 *	WiredTiger LZ4 compression extension - called directly when LZ4 support
 * is built in, or via wiredtiger_extension_init when LZ4 support is included
 * via extension loading.
 */
int
lz4_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	int ret;

	(void)config;    				/* Unused parameters */

	if ((ret = lz_add_compressor(connection, 1, "lz4")) != 0)
		return (ret);
	if ((ret = lz_add_compressor(connection, 0, "lz4-noraw")) != 0)
		return (ret);
	return (0);
}

/*
 * We have to remove this symbol when building as a builtin extension otherwise
 * it will conflict with other builtin libraries.
 */
#ifndef	HAVE_BUILTIN_EXTENSION_LZ4
/*
 * wiredtiger_extension_init --
 *	WiredTiger LZ4 compression extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	return (lz4_extension_init(connection, config));
}
#endif
