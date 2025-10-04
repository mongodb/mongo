// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/zstd.h>

#include "common/zstd_deps.h"

/* Common symbols. zstd_compress must depend on zstd_decompress. */

unsigned int zstd_is_error(size_t code)
{
	return ZSTD_isError(code);
}
EXPORT_SYMBOL(zstd_is_error);

zstd_error_code zstd_get_error_code(size_t code)
{
	return ZSTD_getErrorCode(code);
}
EXPORT_SYMBOL(zstd_get_error_code);

const char *zstd_get_error_name(size_t code)
{
	return ZSTD_getErrorName(code);
}
EXPORT_SYMBOL(zstd_get_error_name);

/* Decompression symbols. */

size_t zstd_dctx_workspace_bound(void)
{
	return ZSTD_estimateDCtxSize();
}
EXPORT_SYMBOL(zstd_dctx_workspace_bound);

zstd_dctx *zstd_init_dctx(void *workspace, size_t workspace_size)
{
	if (workspace == NULL)
		return NULL;
	return ZSTD_initStaticDCtx(workspace, workspace_size);
}
EXPORT_SYMBOL(zstd_init_dctx);

size_t zstd_decompress_dctx(zstd_dctx *dctx, void *dst, size_t dst_capacity,
	const void *src, size_t src_size)
{
	return ZSTD_decompressDCtx(dctx, dst, dst_capacity, src, src_size);
}
EXPORT_SYMBOL(zstd_decompress_dctx);

size_t zstd_dstream_workspace_bound(size_t max_window_size)
{
	return ZSTD_estimateDStreamSize(max_window_size);
}
EXPORT_SYMBOL(zstd_dstream_workspace_bound);

zstd_dstream *zstd_init_dstream(size_t max_window_size, void *workspace,
	size_t workspace_size)
{
	if (workspace == NULL)
		return NULL;
	(void)max_window_size;
	return ZSTD_initStaticDStream(workspace, workspace_size);
}
EXPORT_SYMBOL(zstd_init_dstream);

size_t zstd_reset_dstream(zstd_dstream *dstream)
{
	return ZSTD_resetDStream(dstream);
}
EXPORT_SYMBOL(zstd_reset_dstream);

size_t zstd_decompress_stream(zstd_dstream *dstream, zstd_out_buffer *output,
	zstd_in_buffer *input)
{
	return ZSTD_decompressStream(dstream, output, input);
}
EXPORT_SYMBOL(zstd_decompress_stream);

size_t zstd_find_frame_compressed_size(const void *src, size_t src_size)
{
	return ZSTD_findFrameCompressedSize(src, src_size);
}
EXPORT_SYMBOL(zstd_find_frame_compressed_size);

size_t zstd_get_frame_header(zstd_frame_header *header, const void *src,
	size_t src_size)
{
	return ZSTD_getFrameHeader(header, src, src_size);
}
EXPORT_SYMBOL(zstd_get_frame_header);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Zstd Decompressor");
