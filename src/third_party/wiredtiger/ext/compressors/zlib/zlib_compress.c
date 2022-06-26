/*-
 * Public Domain 2014-present MongoDB, Inc.
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
 * We need to include the configuration file to detect whether this extension is being built into
 * the WiredTiger library; application-loaded compression functions won't need it.
 */
#include <wiredtiger_config.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#ifdef _MSC_VER
#define inline __inline
#endif

/* Local compressor structure. */
typedef struct {
    WT_COMPRESSOR compressor; /* Must come first */

    WT_EXTENSION_API *wt_api; /* Extension API */

    int zlib_level; /* Configuration */
} ZLIB_COMPRESSOR;

/*
 * zlib gives us a cookie to pass to the underlying allocation functions; we need two handles,
 * package them up.
 */
typedef struct {
    WT_COMPRESSOR *compressor;
    WT_SESSION *session;
} ZLIB_OPAQUE;

/*
 * zlib_error --
 *     Output an error message, and return a standard error code.
 */
static int
zlib_error(WT_COMPRESSOR *compressor, WT_SESSION *session, const char *call, int error)
{
    WT_EXTENSION_API *wt_api;

    wt_api = ((ZLIB_COMPRESSOR *)compressor)->wt_api;

    (void)wt_api->err_printf(wt_api, session, "zlib error: %s: %s: %d", call, zError(error), error);
    return (WT_ERROR);
}

/*
 * zalloc --
 *     Allocate a scratch buffer.
 */
static void *
zalloc(void *cookie, uint32_t number, uint32_t size)
{
    ZLIB_OPAQUE *opaque;
    WT_EXTENSION_API *wt_api;

    opaque = cookie;
    wt_api = ((ZLIB_COMPRESSOR *)opaque->compressor)->wt_api;
    return (wt_api->scr_alloc(wt_api, opaque->session, (size_t)number * size));
}

/*
 * zfree --
 *     Free a scratch buffer.
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
 *     WiredTiger zlib compression.
 */
static int
zlib_compress(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *src, size_t src_len,
  uint8_t *dst, size_t dst_len, size_t *result_lenp, int *compression_failed)
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
 *     WiredTiger zlib decompression.
 */
static int
zlib_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *src, size_t src_len,
  uint8_t *dst, size_t dst_len, size_t *result_lenp)
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

    return (ret == Z_OK ? 0 : zlib_error(compressor, session, "inflate", ret));
}

/*
 * zlib_terminate --
 *     WiredTiger zlib compression termination.
 */
static int
zlib_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
    (void)session; /* Unused parameters */

    free(compressor);
    return (0);
}

/*
 * zlib_add_compressor --
 *     Add a zlib compressor.
 */
static int
zlib_add_compressor(WT_CONNECTION *connection, const char *name, int zlib_level)
{
    ZLIB_COMPRESSOR *zlib_compressor;
    int ret;

    if ((zlib_compressor = calloc(1, sizeof(ZLIB_COMPRESSOR))) == NULL)
        return (errno);

    zlib_compressor->compressor.compress = zlib_compress;
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
 *     Handle zlib configuration.
 */
static int
zlib_init_config(WT_CONNECTION *connection, WT_CONFIG_ARG *config, int *zlib_levelp)
{
    WT_CONFIG_ITEM v;
    WT_EXTENSION_API *wt_api;
    int ret, zlib_level;

    /* If configured as a built-in, there's no configuration argument. */
    if (config == NULL)
        return (0);

    /*
     * Zlib compression engine allows applications to specify a compression level; review the
     * configuration.
     */
    wt_api = connection->get_extension_api(connection);
    if ((ret = wt_api->config_get(wt_api, NULL, config, "compression_level", &v)) == 0) {
        /*
         * Between 0-9: level: see zlib manual.
         */
        zlib_level = (int)v.val;
        if (zlib_level < 0 || zlib_level > 9) {
            (void)wt_api->err_printf(
              wt_api, NULL, "zlib_init_config: unsupported compression level %d", zlib_level);
            return (EINVAL);
        }
        *zlib_levelp = zlib_level;
    } else if (ret != WT_NOTFOUND) {
        (void)wt_api->err_printf(
          wt_api, NULL, "zlib_init_config: %s", wt_api->strerror(wt_api, NULL, ret));
        return (ret);
    }

    return (0);
}

int zlib_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);

/*
 * zlib_extension_init --
 *     WiredTiger zlib compression extension - called directly when zlib support is built in, or via
 *     wiredtiger_extension_init when zlib support is included via extension loading.
 */
int
zlib_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    int ret, zlib_level;

    zlib_level = Z_DEFAULT_COMPRESSION; /* Default */
    if ((ret = zlib_init_config(connection, config, &zlib_level)) != 0)
        return (ret);

    if ((ret = zlib_add_compressor(connection, "zlib", zlib_level)) != 0)
        return (ret);

    /* Raw compression API backward compatibility. */
    if ((ret = zlib_add_compressor(connection, "zlib-noraw", zlib_level)) != 0)
        return (ret);
    return (0);
}

/*
 * We have to remove this symbol when building as a builtin extension otherwise it will conflict
 * with other builtin libraries.
 */
#ifndef HAVE_BUILTIN_EXTENSION_ZLIB
/*
 * wiredtiger_extension_init --
 *     WiredTiger zlib compression extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    return (zlib_extension_init(connection, config));
}
#endif
