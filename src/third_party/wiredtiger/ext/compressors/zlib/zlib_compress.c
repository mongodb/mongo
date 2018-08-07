/*-
 * Public Domain 2014-2018 MongoDB, Inc.
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

/*
 * We need to include the configuration file to detect whether this extension
 * is being built into the WiredTiger library; application-loaded compression
 * functions won't need it.
 */
#include <wiredtiger_config.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#ifdef _MSC_VER
#define	inline	__inline
#endif

/* Local compressor structure. */
typedef struct {
	WT_COMPRESSOR compressor;		/* Must come first */

	WT_EXTENSION_API *wt_api;		/* Extension API */

	int zlib_level;				/* Configuration */
} ZLIB_COMPRESSOR;

/*
 * zlib gives us a cookie to pass to the underlying allocation functions; we
 * need two handles, package them up.
 */
typedef struct {
	WT_COMPRESSOR *compressor;
	WT_SESSION *session;
} ZLIB_OPAQUE;

/*
 * zlib_error --
 *	Output an error message, and return a standard error code.
 */
static int
zlib_error(
    WT_COMPRESSOR *compressor, WT_SESSION *session, const char *call, int error)
{
	WT_EXTENSION_API *wt_api;

	wt_api = ((ZLIB_COMPRESSOR *)compressor)->wt_api;

	(void)wt_api->err_printf(wt_api, session,
	    "zlib error: %s: %s: %d", call, zError(error), error);
	return (WT_ERROR);
}

/*
 * zalloc --
 *	Allocate a scratch buffer.
 */
static void *
zalloc(void *cookie, uint32_t number, uint32_t size)
{
	ZLIB_OPAQUE *opaque;
	WT_EXTENSION_API *wt_api;

	opaque = cookie;
	wt_api = ((ZLIB_COMPRESSOR *)opaque->compressor)->wt_api;
	return (wt_api->scr_alloc(
	    wt_api, opaque->session, (size_t)number * size));
}

/*
 * zfree --
 *	Free a scratch buffer.
 */
static void
zfree(void *cookie, void *p)
{
	ZLIB_OPAQUE *opaque;
	WT_EXTENSION_API *wt_api;

	opaque = cookie;
	wt_api = ((ZLIB_COMPRESSOR *)opaque->compressor)->wt_api;
	wt_api->scr_free(wt_api, opaque->session, p);
}

/*
 * zlib_compress --
 *	WiredTiger zlib compression.
 */
static int
zlib_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	ZLIB_COMPRESSOR *zlib_compressor;
	ZLIB_OPAQUE opaque;
	z_stream zs;
	int ret;

	zlib_compressor = (ZLIB_COMPRESSOR *)compressor;

	memset(&zs, 0, sizeof(zs));
	zs.zalloc = zalloc;
	zs.zfree = zfree;
	opaque.compressor = compressor;
	opaque.session = session;
	zs.opaque = &opaque;

	if ((ret = deflateInit(&zs, zlib_compressor->zlib_level)) != Z_OK)
		return (zlib_error(compressor, session, "deflateInit", ret));

	zs.next_in = src;
	zs.avail_in = (uint32_t)src_len;
	zs.next_out = dst;
	zs.avail_out = (uint32_t)dst_len;
	if (deflate(&zs, Z_FINISH) == Z_STREAM_END) {
		*compression_failed = 0;
		*result_lenp = (size_t)zs.total_out;
	} else
		*compression_failed = 1;

	if ((ret = deflateEnd(&zs)) != Z_OK && ret != Z_DATA_ERROR)
		return (zlib_error(compressor, session, "deflateEnd", ret));

	return (0);
}

/*
 * zlib_decompress --
 *	WiredTiger zlib decompression.
 */
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
		return (zlib_error(compressor, session, "inflateInit", ret));

	zs.next_in = src;
	zs.avail_in = (uint32_t)src_len;
	zs.next_out = dst;
	zs.avail_out = (uint32_t)dst_len;
	while ((ret = inflate(&zs, Z_FINISH)) == Z_OK)
		;
	if (ret == Z_STREAM_END) {
		*result_lenp = (size_t)zs.total_out;
		ret = Z_OK;
	}

	if ((tret = inflateEnd(&zs)) != Z_OK && ret == Z_OK)
		ret = tret;

	return (ret == Z_OK ?
	    0 : zlib_error(compressor, session, "inflate", ret));
}

/*
 * zlib_find_slot --
 *	Find the slot containing the target offset (binary search).
 */
static inline uint32_t
zlib_find_slot(uint64_t target, uint32_t *offsets, uint32_t slots)
{
	uint32_t base, indx, limit;

	indx = 1;

	/* Figure out which slot we got to: binary search */
	if (target >= offsets[slots])
		indx = slots;
	else if (target > offsets[1])
		for (base = 2, limit = slots - base; limit != 0; limit >>= 1) {
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
 *	Pack records into a specified on-disk page size.
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
	z_stream *best_zs, *last_zs, _last_zs, *zs, _zs;
	uint32_t curr_slot, last_slot, zlib_reserved;
	bool increase_reserve;
	int ret, tret;

	(void)split_pct;				/* Unused parameters */
	(void)final;

	zlib_compressor = (ZLIB_COMPRESSOR *)compressor;

	/*
	 * Experimentally derived, reserve this many bytes for zlib to finish
	 * up a buffer.  If this isn't sufficient, we don't fail but we will be
	 * inefficient.
	 */
#define	WT_ZLIB_RESERVED	24
#define	WT_ZLIB_RESERVED_MAX	48
	zlib_reserved = WT_ZLIB_RESERVED;

	if (0) {
retry:		/* If we reached our maximum reserve, quit. */
		if (zlib_reserved == WT_ZLIB_RESERVED_MAX)
			return (0);
		zlib_reserved = WT_ZLIB_RESERVED_MAX;
	}

	best_zs = last_zs = NULL;
	last_slot = 0;
	increase_reserve = false;
	ret = 0;

	zs = &_zs;
	memset(zs, 0, sizeof(*zs));
	zs->zalloc = zalloc;
	zs->zfree = zfree;
	opaque.compressor = compressor;
	opaque.session = session;
	zs->opaque = &opaque;

	if ((ret = deflateInit(zs, zlib_compressor->zlib_level)) != Z_OK)
		return (zlib_error(compressor, session, "deflateInit", ret));

	zs->next_in = src;
	zs->next_out = dst;

	/*
	 * Set the target size. The target size is complicated: we don't want
	 * to exceed the smaller of the maximum page size or the destination
	 * buffer length, and in both cases we have to take into account the
	 * space required by zlib to finish up the buffer and the extra bytes
	 * required by our caller.
	 */
	zs->avail_out = (uint32_t)(page_max < dst_len ? page_max : dst_len);
	zs->avail_out -= (uint32_t)(zlib_reserved + extra);

	/*
	 * Strategy: take the available output size and compress that much
	 * input.  Continue until there is no input small enough or the
	 * compression fails to fit.
	 */
	for (;;) {
		/* Find the next slot we will try to compress up to. */
		curr_slot = zlib_find_slot(
		    zs->total_in + zs->avail_out, offsets, slots);
		if (curr_slot > last_slot) {
			zs->avail_in = offsets[curr_slot] - offsets[last_slot];
			while (zs->avail_in > 0 && zs->avail_out > 0)
				if ((ret = deflate(zs, Z_SYNC_FLUSH)) != Z_OK) {
					ret = zlib_error(compressor,
					    session, "deflate", ret);
					goto err;
				}
		}

		/*
		 * We didn't do a deflate, or it didn't work: use the last saved
		 * position (if any).
		 */
		if (curr_slot <= last_slot || zs->avail_in > 0) {
			best_zs = last_zs;
			break;
		}

		/*
		 * If there's more compression to do, save a snapshot and keep
		 * going, otherwise, use the current compression.
		 */
		last_slot = curr_slot;
		if (zs->avail_out > 0) {
			/* Discard any previously saved snapshot. */
			if (last_zs != NULL) {
				ret = deflateEnd(last_zs);
				last_zs = NULL;
				if (ret != Z_OK && ret != Z_DATA_ERROR) {
					ret = zlib_error(compressor,
					    session, "deflateEnd", ret);
					goto err;
				}
			}
			last_zs = &_last_zs;
			if ((ret = deflateCopy(last_zs, zs)) != Z_OK) {
				last_zs = NULL;
				ret = zlib_error(
				    compressor, session, "deflateCopy", ret);
				goto err;
			}
			continue;
		}

		best_zs = zs;
		break;
	}

	if (last_slot > 0 && best_zs != NULL) {
		/* Add the reserved bytes and try to finish the compression. */
		best_zs->avail_out += zlib_reserved;
		ret = deflate(best_zs, Z_FINISH);

		/*
		 * If the end marker didn't fit with the default value, try
		 * again with a maximum value; if that doesn't work, report we
		 * got no work done, WiredTiger will compress the (possibly
		 * large) page image using ordinary compression instead.
		 */
		if (ret == Z_OK || ret == Z_BUF_ERROR) {
			last_slot = 0;
			increase_reserve = true;
		} else if (ret != Z_STREAM_END) {
			ret = zlib_error(
			    compressor, session, "deflate end block", ret);
			goto err;
		}
		ret = 0;
	}

err:	if ((tret = deflateEnd(zs)) != Z_OK && tret != Z_DATA_ERROR)
		ret = zlib_error(compressor, session, "deflateEnd", tret);
	if (last_zs != NULL &&
	    (tret = deflateEnd(last_zs)) != Z_OK && tret != Z_DATA_ERROR)
		ret = zlib_error(compressor, session, "deflateEnd", tret);

	if (ret == 0 && last_slot > 0) {
		*result_slotsp = last_slot;
		*result_lenp = (size_t)best_zs->total_out;
	} else {
		/* We didn't manage to compress anything. */
		*result_slotsp = 0;
		*result_lenp = 1;

		if (increase_reserve)
			goto retry;
	}

#if 0
	/* Decompress the result and confirm it matches the original source. */
	if (ret == 0 && last_slot > 0) {
		WT_EXTENSION_API *wt_api;
		void *decomp;
		size_t result_len;

		wt_api = ((ZLIB_COMPRESSOR *)compressor)->wt_api;

		if ((decomp = zalloc(
		    &opaque, 1, (uint32_t)best_zs->total_in + 100)) == NULL) {
			(void)wt_api->err_printf(wt_api, session,
			    "zlib_compress_raw: zalloc failure");
			return (ENOMEM);
		}
		if ((ret = zlib_decompress(
		    compressor, session, dst, (size_t)best_zs->total_out,
		    decomp, (size_t)best_zs->total_in + 100, &result_len)) == 0)
			if (memcmp(src, decomp, result_len) != 0) {
				(void)wt_api->err_printf(wt_api, session,
				    "zlib_compress_raw: "
				    "deflate compare with original source");
				return (WT_ERROR);
			}
		zfree(&opaque, decomp);
	}
#endif

#if 0
	if (ret == 0 && last_slot > 0)
		fprintf(stderr,
		    "zlib_compress_raw (%s): page_max %" PRIuMAX ", slots %"
		    PRIu32 ", take %" PRIu32 ": %" PRIu32 " -> %" PRIuMAX "\n",
		    final ? "final" : "not final", (uintmax_t)page_max,
		    slots, last_slot, offsets[last_slot],
		    (uintmax_t)*result_lenp);
#endif

	return (ret);
}

/*
 * zlib_terminate --
 *	WiredTiger zlib compression termination.
 */
static int
zlib_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
	(void)session;					/* Unused parameters */

	free(compressor);
	return (0);
}

/*
 * zlib_add_compressor --
 *	Add a zlib compressor.
 */
static int
zlib_add_compressor(
    WT_CONNECTION *connection, bool raw, const char *name, int zlib_level)
{
	ZLIB_COMPRESSOR *zlib_compressor;
	int ret;

	/*
	 * There are two almost identical zlib compressors: one using raw
	 * compression to target a specific block size, and one without.
	 */
	if ((zlib_compressor = calloc(1, sizeof(ZLIB_COMPRESSOR))) == NULL)
		return (errno);

	zlib_compressor->compressor.compress = zlib_compress;
	zlib_compressor->compressor.compress_raw = raw ?
	    zlib_compress_raw : NULL;
	zlib_compressor->compressor.decompress = zlib_decompress;
	zlib_compressor->compressor.pre_size = NULL;
	zlib_compressor->compressor.terminate = zlib_terminate;

	zlib_compressor->wt_api = connection->get_extension_api(connection);
	zlib_compressor->zlib_level = zlib_level;

	/* Load the compressor. */
	if ((ret = connection->add_compressor(
	    connection, name, (WT_COMPRESSOR *)zlib_compressor, NULL)) == 0)
		return (0);

	free(zlib_compressor);
	return (ret);
}

/*
 * zlib_init_config --
 *	Handle zlib configuration.
 */
static int
zlib_init_config(
    WT_CONNECTION *connection, WT_CONFIG_ARG *config, int *zlib_levelp)
{
	WT_CONFIG_ITEM v;
	WT_EXTENSION_API *wt_api;
	int ret, zlib_level;

	/* If configured as a built-in, there's no configuration argument. */
	if (config == NULL)
		return (0);

	/*
	 * Zlib compression engine allows applications to specify a compression
	 * level; review the configuration.
	 */
	wt_api = connection->get_extension_api(connection);
	if ((ret = wt_api->config_get(
	    wt_api, NULL, config, "compression_level", &v)) == 0) {
		/*
		 * Between 0-9: level: see zlib manual.
		 */
		zlib_level = (int)v.val;
		if (zlib_level < 0 || zlib_level > 9) {
			(void)wt_api->err_printf(wt_api, NULL,
			    "zlib_init_config: "
			    "unsupported compression level %d",
			    zlib_level);
			return (EINVAL);
		}
		*zlib_levelp = zlib_level;
	} else if (ret != WT_NOTFOUND) {
		(void)wt_api->err_printf(wt_api, NULL,
		    "zlib_init_config: %s",
		    wt_api->strerror(wt_api, NULL, ret));
		return (ret);
	}

	return (0);
}

int zlib_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);

/*
 * zlib_extension_init --
 *	WiredTiger zlib compression extension - called directly when zlib
 * support is built in, or via wiredtiger_extension_init when zlib support
 * is included via extension loading.
 */
int
zlib_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	int ret, zlib_level;

	zlib_level = Z_DEFAULT_COMPRESSION;		/* Default */
	if ((ret = zlib_init_config(connection, config, &zlib_level)) != 0)
		return (ret);

	if ((ret = zlib_add_compressor(
	    connection, true, "zlib", zlib_level)) != 0)
		return (ret);
	if ((ret = zlib_add_compressor(
	    connection, false, "zlib-noraw", zlib_level)) != 0)
		return (ret);
	return (0);
}

/*
 * We have to remove this symbol when building as a builtin extension otherwise
 * it will conflict with other builtin libraries.
 */
#ifndef	HAVE_BUILTIN_EXTENSION_SNAPPY
/*
 * wiredtiger_extension_init --
 *	WiredTiger zlib compression extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	return (zlib_extension_init(connection, config));
}
#endif
