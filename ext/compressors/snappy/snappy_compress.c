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

#include <snappy-c.h>
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
} SNAPPY_COMPRESSOR;

#ifdef WORDS_BIGENDIAN
/*
 * snappy_bswap64 --
 *	64-bit unsigned little-endian to/from big-endian value.
 */
static inline uint64_t
snappy_bswap64(uint64_t v)
{
	return (
	    ((v << 56) & 0xff00000000000000UL) |
	    ((v << 40) & 0x00ff000000000000UL) |
	    ((v << 24) & 0x0000ff0000000000UL) |
	    ((v <<  8) & 0x000000ff00000000UL) |
	    ((v >>  8) & 0x00000000ff000000UL) |
	    ((v >> 24) & 0x0000000000ff0000UL) |
	    ((v >> 40) & 0x000000000000ff00UL) |
	    ((v >> 56) & 0x00000000000000ffUL)
	);
}
#endif

/*
 * wt_snappy_error --
 *	Output an error message, and return a standard error code.
 */
static int
wt_snappy_error(WT_COMPRESSOR *compressor,
    WT_SESSION *session, const char *call, snappy_status snret)
{
	WT_EXTENSION_API *wt_api;
	const char *msg;

	wt_api = ((SNAPPY_COMPRESSOR *)compressor)->wt_api;

	msg = "unknown snappy status error";
	switch (snret) {
	case SNAPPY_BUFFER_TOO_SMALL:
		msg = "SNAPPY_BUFFER_TOO_SMALL";
		break;
	case SNAPPY_INVALID_INPUT:
		msg = "SNAPPY_INVALID_INPUT";
		break;
	case SNAPPY_OK:
		return (0);
	}

	(void)wt_api->err_printf(wt_api,
	    session, "snappy error: %s: %s: %d", call, msg, snret);
	return (WT_ERROR);
}

/*
 * wt_snappy_compress --
 *	WiredTiger snappy compression.
 */
static int
wt_snappy_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	snappy_status snret;
	size_t snaplen;
	char *snapbuf;

	/*
	 * dst_len was computed in wt_snappy_pre_size, so we know it's big
	 * enough.  Skip past the space we'll use to store the final count
	 * of compressed bytes.
	 */
	snaplen = dst_len - sizeof(size_t);
	snapbuf = (char *)dst + sizeof(size_t);

	/* snaplen is an input and an output arg. */
	snret = snappy_compress((char *)src, src_len, snapbuf, &snaplen);

	if (snret == SNAPPY_OK) {
		if (snaplen + sizeof(size_t) < src_len) {
			*result_lenp = snaplen + sizeof(size_t);
			*compression_failed = 0;

			/*
			 * On decompression, snappy requires an exact compressed
			 * byte count (the current value of snaplen). WiredTiger
			 * does not preserve that value, so save snaplen at the
			 * beginning of the destination buffer.
			 *
			 * Store the value in little-endian format.
			 */
#ifdef WORDS_BIGENDIAN
			snaplen = snappy_bswap64(snaplen);
#endif
			*(size_t *)dst = snaplen;
		} else
			/* The compressor failed to produce a smaller result. */
			*compression_failed = 1;
		return (0);
	}
	return (wt_snappy_error(compressor, session, "snappy_compress", snret));
}

/*
 * wt_snappy_decompress --
 *	WiredTiger snappy decompression.
 */
static int
wt_snappy_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	WT_EXTENSION_API *wt_api;
	snappy_status snret;
	size_t snaplen;

	wt_api = ((SNAPPY_COMPRESSOR *)compressor)->wt_api;

	/*
	 * Retrieve the saved length, handling little- to big-endian conversion
	 * as necessary.
	 */
	snaplen = *(size_t *)src;
#ifdef WORDS_BIGENDIAN
	snaplen = snappy_bswap64(snaplen);
#endif
	if (snaplen + sizeof(size_t) > src_len) {
		(void)wt_api->err_printf(wt_api,
		    session,
		    "wt_snappy_decompress: stored size exceeds buffer size");
		return (WT_ERROR);
	}

	/* dst_len is an input and an output arg. */
	snret = snappy_uncompress(
	    (char *)src + sizeof(size_t), snaplen, (char *)dst, &dst_len);

	if (snret == SNAPPY_OK) {
		*result_lenp = dst_len;
		return (0);
	}

	return (
	    wt_snappy_error(compressor, session, "snappy_decompress", snret));
}

/*
 * wt_snappy_pre_size --
 *	WiredTiger snappy destination buffer sizing.
 */
static int
wt_snappy_pre_size(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    size_t *result_lenp)
{
	(void)compressor;			/* Unused parameters */
	(void)session;
	(void)src;

	/*
	 * Snappy requires the dest buffer be somewhat larger than the source.
	 * Fortunately, this is fast to compute, and will give us a dest buffer
	 * in wt_snappy_compress that we can compress to directly.  We add space
	 * in the dest buffer to store the accurate compressed size.
	 */
	*result_lenp = snappy_max_compressed_length(src_len) + sizeof(size_t);
	return (0);
}

/*
 * wt_snappy_terminate --
 *	WiredTiger snappy compression termination.
 */
static int
wt_snappy_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
	(void)session;				/* Unused parameters */

	free(compressor);
	return (0);
}

int snappy_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);

/*
 * snappy_extension_init --
 *	WiredTiger snappy compression extension - called directly when
 *	Snappy support is built in, or via wiredtiger_extension_init when
 *	snappy support is included via extension loading.
 */
int
snappy_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	SNAPPY_COMPRESSOR *snappy_compressor;

	(void)config;				/* Unused parameters */

	if ((snappy_compressor = calloc(1, sizeof(SNAPPY_COMPRESSOR))) == NULL)
		return (errno);

	snappy_compressor->compressor.compress = wt_snappy_compress;
	snappy_compressor->compressor.compress_raw = NULL;
	snappy_compressor->compressor.decompress = wt_snappy_decompress;
	snappy_compressor->compressor.pre_size = wt_snappy_pre_size;
	snappy_compressor->compressor.terminate = wt_snappy_terminate;

	snappy_compressor->wt_api = connection->get_extension_api(connection);

	return (connection->add_compressor(
	    connection, "snappy", (WT_COMPRESSOR *)snappy_compressor, NULL));
}

/*
 * We have to remove this symbol when building as a builtin extension otherwise
 * it will conflict with other builtin libraries.
 */
#ifndef	HAVE_BUILTIN_EXTENSION_SNAPPY
/*
 * wiredtiger_extension_init --
 *	WiredTiger snappy compression extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	return snappy_extension_init(connection, config);
}
#endif
