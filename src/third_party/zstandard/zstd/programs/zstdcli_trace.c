/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "zstdcli_trace.h"

#include <stdio.h>
#include <stdlib.h>

#include "timefn.h"
#include "util.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "../lib/zstd.h"
/* We depend on the trace header to avoid duplicating the ZSTD_trace struct.
 * But, we check the version so it is compatible with dynamic linking.
 */
#include "../lib/common/zstd_trace.h"
/* We only use macros from threading.h so it is compatible with dynamic linking */
#include "../lib/common/threading.h"

#if ZSTD_TRACE

static FILE* g_traceFile = NULL;
static int g_mutexInit = 0;
static ZSTD_pthread_mutex_t g_mutex;
static UTIL_time_t g_enableTime = UTIL_TIME_INITIALIZER;

void TRACE_enable(char const* filename)
{
    int const writeHeader = !UTIL_isRegularFile(filename);
    if (g_traceFile)
        fclose(g_traceFile);
    g_traceFile = fopen(filename, "a");
    if (g_traceFile && writeHeader) {
        /* Fields:
        * algorithm
        * version
        * method
        * streaming
        * level
        * workers
        * dictionary size
        * uncompressed size
        * compressed size
        * duration nanos
        * compression ratio
        * speed MB/s
        */
        fprintf(g_traceFile, "Algorithm, Version, Method, Mode, Level, Workers, Dictionary Size, Uncompressed Size, Compressed Size, Duration Nanos, Compression Ratio, Speed MB/s\n");
    }
    g_enableTime = UTIL_getTime();
    if (!g_mutexInit) {
        if (!ZSTD_pthread_mutex_init(&g_mutex, NULL)) {
            g_mutexInit = 1;
        } else {
            TRACE_finish();
        }
    }
}

void TRACE_finish(void)
{
    if (g_traceFile) {
        fclose(g_traceFile);
    }
    g_traceFile = NULL;
    if (g_mutexInit) {
        ZSTD_pthread_mutex_destroy(&g_mutex);
        g_mutexInit = 0;
    }
}

static void TRACE_log(char const* method, PTime duration, ZSTD_Trace const* trace)
{
    int level = 0;
    int workers = 0;
    double const ratio = (double)trace->uncompressedSize / (double)trace->compressedSize;
    double const speed = ((double)trace->uncompressedSize * 1000) / (double)duration;
    if (trace->params) {
        ZSTD_CCtxParams_getParameter(trace->params, ZSTD_c_compressionLevel, &level);
        ZSTD_CCtxParams_getParameter(trace->params, ZSTD_c_nbWorkers, &workers);
    }
    assert(g_traceFile != NULL);

    ZSTD_pthread_mutex_lock(&g_mutex);
    /* Fields:
     * algorithm
     * version
     * method
     * streaming
     * level
     * workers
     * dictionary size
     * uncompressed size
     * compressed size
     * duration nanos
     * compression ratio
     * speed MB/s
     */
    fprintf(g_traceFile,
        "zstd, %u, %s, %s, %d, %d, %llu, %llu, %llu, %llu, %.2f, %.2f\n",
        trace->version,
        method,
        trace->streaming ? "streaming" : "single-pass",
        level,
        workers,
        (unsigned long long)trace->dictionarySize,
        (unsigned long long)trace->uncompressedSize,
        (unsigned long long)trace->compressedSize,
        (unsigned long long)duration,
        ratio,
        speed);
    ZSTD_pthread_mutex_unlock(&g_mutex);
}

/**
 * These symbols override the weak symbols provided by the library.
 */

ZSTD_TraceCtx ZSTD_trace_compress_begin(ZSTD_CCtx const* cctx)
{
    (void)cctx;
    if (g_traceFile == NULL)
        return 0;
    return (ZSTD_TraceCtx)UTIL_clockSpanNano(g_enableTime);
}

void ZSTD_trace_compress_end(ZSTD_TraceCtx ctx, ZSTD_Trace const* trace)
{
    PTime const beginNanos = (PTime)ctx;
    PTime const endNanos = UTIL_clockSpanNano(g_enableTime);
    PTime const durationNanos = endNanos > beginNanos ? endNanos - beginNanos : 0;
    assert(g_traceFile != NULL);
    assert(trace->version == ZSTD_VERSION_NUMBER); /* CLI version must match. */
    TRACE_log("compress", durationNanos, trace);
}

ZSTD_TraceCtx ZSTD_trace_decompress_begin(ZSTD_DCtx const* dctx)
{
    (void)dctx;
    if (g_traceFile == NULL)
        return 0;
    return (ZSTD_TraceCtx)UTIL_clockSpanNano(g_enableTime);
}

void ZSTD_trace_decompress_end(ZSTD_TraceCtx ctx, ZSTD_Trace const* trace)
{
    PTime const beginNanos = (PTime)ctx;
    PTime const endNanos = UTIL_clockSpanNano(g_enableTime);
    PTime const durationNanos = endNanos > beginNanos ? endNanos - beginNanos : 0;
    assert(g_traceFile != NULL);
    assert(trace->version == ZSTD_VERSION_NUMBER); /* CLI version must match. */
    TRACE_log("decompress", durationNanos, trace);
}

#else /* ZSTD_TRACE */

void TRACE_enable(char const* filename)
{
    (void)filename;
}

void TRACE_finish(void) {}

#endif /* ZSTD_TRACE */
