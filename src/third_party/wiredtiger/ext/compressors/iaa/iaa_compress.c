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

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * We need to include the configuration file to detect whether this extension is being built into
 * the WiredTiger library; application-loaded compression functions won't need it.
 */
#include <wiredtiger_config.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>
#include <time.h>
#include "iaaInterface-c.h"

#ifdef _MSC_VER
#define inline __inline
#endif

/* Local compressor structure. */
typedef struct {
    WT_COMPRESSOR compressor; /* Must come first */

    WT_EXTENSION_API *wt_api; /* Extension API */
} iaa_COMPRESSOR;

/*
 * iaa_error --
 *     Output an error message, and return a standard error code.
 */
static int
iaa_error(WT_COMPRESSOR *compressor, WT_SESSION *session, const char *call)
{
    WT_EXTENSION_API *wt_api;

    wt_api = ((iaa_COMPRESSOR *)compressor)->wt_api;

    (void)wt_api->err_printf(wt_api, session, "iaa error: %s", call);
    return (WT_ERROR);
}

/*
 * iaa_compress --
 *     WiredTiger iaa compression.
 */
static int
iaa_compress(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *src, size_t src_len,
  uint8_t *dst, size_t dst_len, size_t *result_lenp, int *compression_failed)
{
    uint32_t result_len =
      doCompressData(compressor, session, src, (uint32_t)src_len, dst, (uint32_t)dst_len);
    if (result_len > 0) {
        *compression_failed = 0;
        *result_lenp = (size_t)result_len;
        return (0);
    }
    *compression_failed = 1;
    return (iaa_error(compressor, session, "iaa_compress"));
}

/*
 * iaa_decompress --
 *     WiredTiger iaa decompression.
 */
static int
iaa_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *src, size_t src_len,
  uint8_t *dst, size_t dst_len, size_t *result_lenp)
{
    uint32_t result_len;
    doDecompressData(
      compressor, session, src, (uint32_t)src_len, dst, (uint32_t)dst_len, &result_len);
    if (result_len > 0) {
        *result_lenp = (size_t)result_len;
        return (0);
    }
    return (iaa_error(compressor, session, "iaa_decompress"));
}

/*
 * iaa_terminate --
 *     WiredTiger iaa compression termination.
 */
static int
iaa_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
    (void)session; /* Unused parameters */

    free(compressor);
    return (0);
}

/*
 * iaa_pre_size --
 *     WiredTiger iaa destination buffer sizing.
 */
static int
iaa_pre_size(
  WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *src, size_t src_len, size_t *result_lenp)
{
    (void)compressor; /* Unused parameters */
    (void)session;
    (void)src;

    /*
     * Get the upper-bound of the buffer size needed by the compression.
     */
    *result_lenp = getMaxCompressedDataSize((uint32_t)src_len);
    return (0);
}

/*
 * iaa_add_compressor --
 *     Add a iaa compressor.
 */
static int
iaa_add_compressor(WT_CONNECTION *connection, const char *name)
{
    iaa_COMPRESSOR *iaa_compressor;
    int ret;
    if ((iaa_compressor = calloc(1, sizeof(iaa_COMPRESSOR))) == NULL)
        return (errno);

    iaa_compressor->compressor.compress = iaa_compress;
    iaa_compressor->compressor.decompress = iaa_decompress;
    iaa_compressor->compressor.pre_size = iaa_pre_size;
    iaa_compressor->compressor.terminate = iaa_terminate;

    iaa_compressor->wt_api = connection->get_extension_api(connection);

    /* Load the compressor. */
    if ((ret = connection->add_compressor(
           connection, name, (WT_COMPRESSOR *)iaa_compressor, NULL)) == 0) {
        return (0);
    }

    free(iaa_compressor);
    return (ret);
}

int iaa_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);

/*
 * iaa_extension_init --
 *     WiredTiger iaa compression extension - called directly when iaa support is built in, or via
 *     wiredtiger_extension_init when iaa support is included via extension loading.
 */
int
iaa_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    (void)config;

    return (iaa_add_compressor(connection, "iaa"));
}

/*
 * We have to remove this symbol when building as a builtin extension otherwise it will conflict
 * with other builtin libraries.
 */
#ifndef HAVE_BUILTIN_EXTENSION_IAA
/*
 * wiredtiger_extension_init --
 *     WiredTiger iaa compression extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    return (iaa_extension_init(connection, config));
}
#endif
