/*-
 * Public Domain 2014-2015 MongoDB, Inc.
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

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

/*
 * For the wiredtiger library we expect to use the system library.  When
 * wiredtiger is built as part of MongoDB, we use the lz4 source from
 * https://github.com/Cyan4973/lz4 (LZ4 implementation in C).
 *
 * This code is licensed under the BSD 3-Clause License.
 */
#include <lz4.h>

/*
 * We need to include the configuration file to detect whether this extension
 * is being built into the WiredTiger library.
 */
#include "wiredtiger_config.h"

/* Local compressor structure. */
typedef struct {
	WT_COMPRESSOR compressor;		/* Must come first */
	WT_EXTENSION_API *wt_api;		/* Extension API */
} LZ4_COMPRESSOR;

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
	(void)session;    /* Unused parameters */

	*compression_failed = 0;
	if (dst_len < src_len + 8) {
	/*
	 *  do not attempt but should not happen with prior call to lz4_pre_size
	 *  TODO: consider change to assert
	 */
		*compression_failed = 1;
		return (0);
	}

	/*
	 *  Store the length of the compressed block in the first
	 *  sizeof(size_t) bytes.  We will skip past the length value to store
	 *  the compressed bytes.
	 */
	char *buf = (char *)dst + sizeof(size_t);

	/*
	 * Call LZ4 to compress
	 */
	int lz4_len = LZ4_compress((const char *)src, buf, src_len);
	*result_lenp = lz4_len;
	*(size_t *)dst = (size_t)lz4_len;

	/*
	 * Return the compressed data length, including our size_t compressed
	 * data byte count
	 */
	*result_lenp = lz4_len + sizeof(size_t);

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
	(void)session;				/* Unused parameters */
	(void)src_len;

	/*
	 * Retrieve compressed data length from start of compressed data buffer
	 */
	size_t src_data_len = *(size_t *)src;

	/* Skip over sizeof(size_t) bytes for actual start of compressed data */
	char *compressed_data = (char *)src + sizeof(size_t);

	/*
	 *  The destination buffer length should always be sufficient because
	 *  wiredtiger keeps track of the byte count before compression
	 */

	/* Call LZ4 to decompress */
	int decoded = LZ4_decompress_safe(
	    compressed_data, (char *)dst, src_data_len, dst_len);
	if (decoded < 0) {
		WT_EXTENSION_API *wt_api =
		    ((LZ4_COMPRESSOR *)compressor)->wt_api;
		(void)wt_api->err_printf(wt_api,
		    session, "LZ4 error: LZ4_decompress_safe: %d", decoded);
		return (WT_ERROR);
	}

	size_t decompressed_data_len = decoded;
	/* return the uncompressed data length */
	*result_lenp = decompressed_data_len;

	return (0);
}

/*
 * lz4_pre_size --
 *	WiredTiger LZ4 destination buffer sizing for compression.
 */
static int
lz4_pre_size(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    size_t *result_lenp)
{
	size_t dst_buffer_len_needed;

	WT_UNUSED(session);
	WT_UNUSED(src);

	/*
	 *  We must reserve a little extra space for our compressed data length
	 *  value stored at the start of the compressed data buffer.  Random
	 *  data doesn't compress well and we could overflow the destination
	 *  buffer.
	 */
	dst_buffer_len_needed = src_len + sizeof(size_t);

	/* return the buffer length needed */
	*result_lenp = dst_buffer_len_needed;
	return (0);
}

/*
 * lz4_terminate --
 *	WiredTiger LZ4 compression termination.
 */
static int
lz4_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
	WT_UNUSED(session);

	/* Free the allocated memory. */
	free(compressor);

	return (0);
}

/*
 * wiredtiger_extension_init --
 *	A simple shared library compression example.
 */
int
lz4_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	LZ4_COMPRESSOR *lz4_compressor;

	(void)config;    /* Unused parameters */

	if ((lz4_compressor = calloc(1, sizeof(LZ4_COMPRESSOR))) == NULL)
		return (errno);

	/*
	 * Allocate a local compressor structure, with a WT_COMPRESSOR structure
	 * as the first field, allowing us to treat references to either type of
	 * structure as a reference to the other type.
	 *
	 * This could be simplified if only a single database is opened in the
	 * application, we could use a static WT_COMPRESSOR structure, and a
	 * static reference to the WT_EXTENSION_API methods, then we don't need
	 * to allocate memory when the compressor is initialized or free it when
	 * the compressor is terminated.  However, this approach is more general
	 * purpose and supports multiple databases per application.
	 */
	lz4_compressor->compressor.compress = lz4_compress;
	lz4_compressor->compressor.compress_raw = NULL;
	lz4_compressor->compressor.decompress = lz4_decompress;
	lz4_compressor->compressor.pre_size = lz4_pre_size;
	lz4_compressor->compressor.terminate = lz4_terminate;

	lz4_compressor->wt_api = connection->get_extension_api(connection);

	/* Load the compressor */
	return (connection->add_compressor(
	    connection, "lz4", (WT_COMPRESSOR *)lz4_compressor, NULL));
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
	return lz4_extension_init(connection, config);
}
#endif
