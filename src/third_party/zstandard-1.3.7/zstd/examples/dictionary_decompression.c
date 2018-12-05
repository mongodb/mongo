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
    perror(filename);
    exit(1);
}

static FILE* fopen_orDie(const char *filename, const char *instruction)
{
    FILE* const inFile = fopen(filename, instruction);
    if (inFile) return inFile;
    /* error */
    perror(filename);
    exit(2);
}

static void* malloc_orDie(size_t size)
{
    void* const buff = malloc(size);
    if (buff) return buff;
    /* error */
    perror("malloc");
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
    fclose(inFile);
    *size = buffSize;
    return buffer;
}

/* createDict() :
   `dictFileName` is supposed to have been created using `zstd --train` */
static ZSTD_DDict* createDict_orDie(const char* dictFileName)
{
    size_t dictSize;
    printf("loading dictionary %s \n", dictFileName);
    void* const dictBuffer = loadFile_orDie(dictFileName, &dictSize);
    ZSTD_DDict* const ddict = ZSTD_createDDict(dictBuffer, dictSize);
    if (ddict==NULL) { fprintf(stderr, "ZSTD_createDDict error \n"); exit(5); }
    free(dictBuffer);
    return ddict;
}


static void decompress(const char* fname, const ZSTD_DDict* ddict)
{
    size_t cSize;
    void* const cBuff = loadFile_orDie(fname, &cSize);
    unsigned long long const rSize = ZSTD_findDecompressedSize(cBuff, cSize);
    if (rSize==ZSTD_CONTENTSIZE_ERROR) {
        fprintf(stderr, "%s : it was not compressed by zstd.\n", fname);
        exit(5);
    } else if (rSize==ZSTD_CONTENTSIZE_UNKNOWN) {
        fprintf(stderr, "%s : original size unknown \n", fname);
        exit(6);
    }

    void* const rBuff = malloc_orDie((size_t)rSize);

    ZSTD_DCtx* const dctx = ZSTD_createDCtx();
    if (dctx==NULL) { fprintf(stderr, "ZSTD_createDCtx() error \n"); exit(10); }
    size_t const dSize = ZSTD_decompress_usingDDict(dctx, rBuff, rSize, cBuff, cSize, ddict);
    if (dSize != rSize) {
        fprintf(stderr, "error decoding %s : %s \n", fname, ZSTD_getErrorName(dSize));
        exit(7);
    }

    /* success */
    printf("%25s : %6u -> %7u \n", fname, (unsigned)cSize, (unsigned)rSize);

    ZSTD_freeDCtx(dctx);
    free(rBuff);
    free(cBuff);
}


int main(int argc, const char** argv)
{
    const char* const exeName = argv[0];

    if (argc<3) {
        printf("wrong arguments\n");
        printf("usage:\n");
        printf("%s [FILES] dictionary\n", exeName);
        return 1;
    }

    /* load dictionary only once */
    const char* const dictName = argv[argc-1];
    ZSTD_DDict* const dictPtr = createDict_orDie(dictName);

    int u;
    for (u=1; u<argc-1; u++) decompress(argv[u], dictPtr);

    ZSTD_freeDDict(dictPtr);
    printf("All %u files correctly decoded (in memory) \n", argc-2);
    return 0;
}
