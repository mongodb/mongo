/*
 * Copyright (c) Martin Liska, SUSE, Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


#include <stdio.h>     // printf
#include <stdlib.h>    // free
#include <string.h>    // memset, strcat, strlen
#include <zstd.h>      // presumes zstd library is installed
#include "common.h"    // Helper functions, CHECK(), and CHECK_ZSTD()
#include <pthread.h>

typedef struct compress_args
{
  const char *fname;
  char *outName;
  int cLevel;
#if defined(ZSTD_STATIC_LINKING_ONLY)
  ZSTD_threadPool *pool;
#endif
} compress_args_t;

static void *compressFile_orDie(void *data)
{
    const int nbThreads = 16;

    compress_args_t *args = (compress_args_t *)data;
    fprintf (stderr, "Starting compression of %s with level %d, using %d threads\n", args->fname, args->cLevel, nbThreads);
    /* Open the input and output files. */
    FILE* const fin  = fopen_orDie(args->fname, "rb");
    FILE* const fout = fopen_orDie(args->outName, "wb");
    /* Create the input and output buffers.
     * They may be any size, but we recommend using these functions to size them.
     * Performance will only suffer significantly for very tiny buffers.
     */
    size_t const buffInSize = ZSTD_CStreamInSize();
    void*  const buffIn  = malloc_orDie(buffInSize);
    size_t const buffOutSize = ZSTD_CStreamOutSize();
    void*  const buffOut = malloc_orDie(buffOutSize);

    /* Create the context. */
    ZSTD_CCtx* const cctx = ZSTD_createCCtx();
    CHECK(cctx != NULL, "ZSTD_createCCtx() failed!");

#if defined(ZSTD_STATIC_LINKING_ONLY)
    size_t r = ZSTD_CCtx_refThreadPool(cctx, args->pool);
    CHECK(r == 0, "ZSTD_CCtx_refThreadPool failed!");
#endif

    /* Set any parameters you want.
     * Here we set the compression level, and enable the checksum.
     */
    CHECK_ZSTD( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, args->cLevel) );
    CHECK_ZSTD( ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1) );
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, nbThreads);

    /* This loop reads from the input file, compresses that entire chunk,
     * and writes all output produced to the output file.
     */
    size_t const toRead = buffInSize;
    for (;;) {
        size_t read = fread_orDie(buffIn, toRead, fin);
        /* Select the flush mode.
         * If the read may not be finished (read == toRead) we use
         * ZSTD_e_continue. If this is the last chunk, we use ZSTD_e_end.
         * Zstd optimizes the case where the first flush mode is ZSTD_e_end,
         * since it knows it is compressing the entire source in one pass.
         */
        int const lastChunk = (read < toRead);
        ZSTD_EndDirective const mode = lastChunk ? ZSTD_e_end : ZSTD_e_continue;
        /* Set the input buffer to what we just read.
         * We compress until the input buffer is empty, each time flushing the
         * output.
         */
        ZSTD_inBuffer input = { buffIn, read, 0 };
        int finished;
        do {
            /* Compress into the output buffer and write all of the output to
             * the file so we can reuse the buffer next iteration.
             */
            ZSTD_outBuffer output = { buffOut, buffOutSize, 0 };
            size_t const remaining = ZSTD_compressStream2(cctx, &output , &input, mode);
            CHECK_ZSTD(remaining);
            fwrite_orDie(buffOut, output.pos, fout);
            /* If we're on the last chunk we're finished when zstd returns 0,
             * which means its consumed all the input AND finished the frame.
             * Otherwise, we're finished when we've consumed all the input.
             */
            finished = lastChunk ? (remaining == 0) : (input.pos == input.size);
        } while (!finished);
        CHECK(input.pos == input.size,
              "Impossible: zstd only returns 0 when the input is completely consumed!");

        if (lastChunk) {
            break;
        }
    }

    fprintf (stderr, "Finishing compression of %s\n", args->outName);

    ZSTD_freeCCtx(cctx);
    fclose_orDie(fout);
    fclose_orDie(fin);
    free(buffIn);
    free(buffOut);
    free(args->outName);

    return NULL;
}


static char* createOutFilename_orDie(const char* filename)
{
    size_t const inL = strlen(filename);
    size_t const outL = inL + 5;
    void* const outSpace = malloc_orDie(outL);
    memset(outSpace, 0, outL);
    strcat(outSpace, filename);
    strcat(outSpace, ".zst");
    return (char*)outSpace;
}

int main(int argc, const char** argv)
{
    const char* const exeName = argv[0];

    if (argc<=3) {
        printf("wrong arguments\n");
        printf("usage:\n");
        printf("%s POOL_SIZE LEVEL FILES\n", exeName);
        return 1;
    }

    int pool_size = atoi (argv[1]);
    CHECK(pool_size != 0, "can't parse POOL_SIZE!");

    int level = atoi (argv[2]);
    CHECK(level != 0, "can't parse LEVEL!");

    argc -= 3;
    argv += 3;

#if defined(ZSTD_STATIC_LINKING_ONLY)
    ZSTD_threadPool *pool = ZSTD_createThreadPool (pool_size);
    CHECK(pool != NULL, "ZSTD_createThreadPool() failed!");
    fprintf (stderr, "Using shared thread pool of size %d\n", pool_size);
#else
    fprintf (stderr, "All threads use its own thread pool\n");
#endif

    pthread_t *threads = malloc_orDie(argc * sizeof(pthread_t));
    compress_args_t *args = malloc_orDie(argc * sizeof(compress_args_t));

    for (unsigned i = 0; i < argc; i++)
    {
      args[i].fname = argv[i];
      args[i].outName = createOutFilename_orDie(args[i].fname);
      args[i].cLevel = level;
#if defined(ZSTD_STATIC_LINKING_ONLY)
      args[i].pool = pool;
#endif

      pthread_create (&threads[i], NULL, compressFile_orDie, &args[i]);
    }

    for (unsigned i = 0; i < argc; i++)
      pthread_join (threads[i], NULL);

#if defined(ZSTD_STATIC_LINKING_ONLY)
    ZSTD_freeThreadPool (pool);
#endif

    return 0;
}
