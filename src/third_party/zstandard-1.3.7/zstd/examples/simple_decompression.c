/*
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include <stdlib.h>    // malloc, exit
#include <stdio.h>     // printf
#include <string.h>    // strerror
#include <errno.h>     // errno
#include <sys/stat.h>  // stat
#define ZSTD_STATIC_LINKING_ONLY   // ZSTD_findDecompressedSize
#include <zstd.h>      // presumes zstd library is installed


static off_t fsize_orDie(const char *filename)
{
    struct stat st;
    if (stat(filename, &st) == 0) return st.st_size;
    /* error */
    fprintf(stderr, "stat: %s : %s \n", filename, strerror(errno));
    exit(1);
}

static FILE* fopen_orDie(const char *filename, const char *instruction)
{
    FILE* const inFile = fopen(filename, instruction);
    if (inFile) return inFile;
    /* error */
    fprintf(stderr, "fopen: %s : %s \n", filename, strerror(errno));
    exit(2);
}

static void* malloc_orDie(size_t size)
{
    void* const buff = malloc(size + !size);   /* avoid allocating size of 0 : may return NULL (implementation dependent) */
    if (buff) return buff;
    /* error */
    fprintf(stderr, "malloc: %s \n", strerror(errno));
    exit(3);
}

static void* loadFile_orDie(const char* fileName, size_t* size)
{
    off_t const buffSize = fsize_orDie(fileName);
    FILE* const inFile = fopen_orDie(fileName, "rb");
    void* const buffer = malloc_orDie(buffSize);
    size_t const readSize = fread(buffer, 1, buffSize, inFile);
    if (readSize != (size_t)buffSize) {
        fprintf(stderr, "fread: %s : %s \n", fileName, strerror(errno));
        exit(4);
    }
    fclose(inFile);   /* can't fail (read only) */
    *size = buffSize;
    return buffer;
}


static void decompress(const char* fname)
{
    size_t cSize;
    void* const cBuff = loadFile_orDie(fname, &cSize);
    unsigned long long const rSize = ZSTD_findDecompressedSize(cBuff, cSize);
    if (rSize==ZSTD_CONTENTSIZE_ERROR) {
        fprintf(stderr, "%s : it was not compressed by zstd.\n", fname);
        exit(5);
    } else if (rSize==ZSTD_CONTENTSIZE_UNKNOWN) {
        fprintf(stderr,
                "%s : original size unknown. Use streaming decompression instead.\n", fname);
        exit(6);
    }

    void* const rBuff = malloc_orDie((size_t)rSize);

    size_t const dSize = ZSTD_decompress(rBuff, rSize, cBuff, cSize);

    if (dSize != rSize) {
        fprintf(stderr, "error decoding %s : %s \n", fname, ZSTD_getErrorName(dSize));
        exit(7);
    }

    /* success */
    printf("%25s : %6u -> %7u \n", fname, (unsigned)cSize, (unsigned)rSize);

    free(rBuff);
    free(cBuff);
}


int main(int argc, const char** argv)
{
    const char* const exeName = argv[0];

    if (argc!=2) {
        printf("wrong arguments\n");
        printf("usage:\n");
        printf("%s FILE\n", exeName);
        return 1;
    }

    decompress(argv[1]);

    printf("%s correctly decoded (in memory). \n", argv[1]);

    return 0;
}
