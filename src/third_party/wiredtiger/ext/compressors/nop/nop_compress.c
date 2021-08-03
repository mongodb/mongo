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
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

/*! [WT_COMPRESSOR initialization structure] */
/* Local compressor structure. */
typedef struct {
    WT_COMPRESSOR compressor; /* Must come first */

    WT_EXTENSION_API *wt_api; /* Extension API */

    unsigned long nop_calls; /* Count of calls */

} NOP_COMPRESSOR;
/*! [WT_COMPRESSOR initialization structure] */

/*! [WT_COMPRESSOR compress] */
/*
 * nop_compress --
 *     A simple compression example that passes data through unchanged.
 */
static int
nop_compress(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *src, size_t src_len,
  uint8_t *dst, size_t dst_len, size_t *result_lenp, int *compression_failed)
{
    NOP_COMPRESSOR *nop_compressor = (NOP_COMPRESSOR *)compressor;

    (void)session; /* Unused parameters */

    ++nop_compressor->nop_calls; /* Call count */

    *compression_failed = 0;
    if (dst_len < src_len) {
        *compression_failed = 1;
        return (0);
    }

    memcpy(dst, src, src_len);
    *result_lenp = src_len;

    return (0);
}
/*! [WT_COMPRESSOR compress] */

/*! [WT_COMPRESSOR decompress] */
/*
 * nop_decompress --
 *     A simple decompression example that passes data through unchanged.
 */
static int
nop_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *src, size_t src_len,
  uint8_t *dst, size_t dst_len, size_t *result_lenp)
{
    NOP_COMPRESSOR *nop_compressor = (NOP_COMPRESSOR *)compressor;

    (void)session; /* Unused parameters */
    (void)src_len;

    ++nop_compressor->nop_calls; /* Call count */

    /*
     * The destination length is the number of uncompressed bytes we're expected to return.
     */
    memcpy(dst, src, dst_len);
    *result_lenp = dst_len;
    return (0);
}
/*! [WT_COMPRESSOR decompress] */

/*! [WT_COMPRESSOR presize] */
/*
 * nop_pre_size --
 *     A simple pre-size example that returns the source length.
 */
static int
nop_pre_size(
  WT_COMPRESSOR *compressor, WT_SESSION *session, uint8_t *src, size_t src_len, size_t *result_lenp)
{
    NOP_COMPRESSOR *nop_compressor = (NOP_COMPRESSOR *)compressor;

    (void)session; /* Unused parameters */
    (void)src;

    ++nop_compressor->nop_calls; /* Call count */

    *result_lenp = src_len;
    return (0);
}
/*! [WT_COMPRESSOR presize] */

/*! [WT_COMPRESSOR terminate] */
/*
 * nop_terminate --
 *     WiredTiger no-op compression termination.
 */
static int
nop_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
    NOP_COMPRESSOR *nop_compressor = (NOP_COMPRESSOR *)compressor;

    (void)session; /* Unused parameters */

    ++nop_compressor->nop_calls; /* Call count */

    /* Free the allocated memory. */
    free(compressor);

    return (0);
}
/*! [WT_COMPRESSOR terminate] */

/*! [WT_COMPRESSOR initialization function] */
/*
 * wiredtiger_extension_init --
 *     A simple shared library compression example.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    NOP_COMPRESSOR *nop_compressor;
    int ret;

    (void)config; /* Unused parameters */

    if ((nop_compressor = calloc(1, sizeof(NOP_COMPRESSOR))) == NULL)
        return (errno);

    /*
     * Allocate a local compressor structure, with a WT_COMPRESSOR structure as the first field,
     * allowing us to treat references to either type of structure as a reference to the other type.
     *
     * Heap memory (not static), because it can support multiple databases.
     */
    nop_compressor->compressor.compress = nop_compress;
    nop_compressor->compressor.decompress = nop_decompress;
    nop_compressor->compressor.pre_size = nop_pre_size;
    nop_compressor->compressor.terminate = nop_terminate;

    nop_compressor->wt_api = connection->get_extension_api(connection);

    /* Load the compressor */
    if ((ret = connection->add_compressor(
           connection, "nop", (WT_COMPRESSOR *)nop_compressor, NULL)) == 0)
        return (0);

    free(nop_compressor);
    return (ret);
}
/*! [WT_COMPRESSOR initialization function] */
