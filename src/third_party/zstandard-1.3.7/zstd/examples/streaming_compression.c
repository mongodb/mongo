/*
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


#include <stdlib.h>    // malloc, free, exit
#include <stdio.h>     // fprintf, perror, feof, fopen, etc.
#include <string.h>    // strlen, memset, strcat
#include <zstd.h>      // presumes zstd library is installed


static void* malloc_orDie(size_t size)
{
    void* const buff = malloc(size);
    if (buff) return buff;
    /* error */
    perror("malloc:");
    exit(1);
}

static FILE* fopen_orDie(const char *filename, const char *instruction)
{
    FILE* const inFile = fopen(filename, instruction);
    if (inFile) return inFile;
    /* error */
    perror(filename);
    exit(3);
}

static size_t fread_orDie(void* buffer, size_t sizeToRead, FILE* file)
{
    size_t const readSize = fread(buffer, 1, sizeToRead, file);
    if (readSize == sizeToRead) return readSize;   /* good */
    if (feof(file)) return readSize;   /* good, reached end of file */
    /* error */
    perror("fread");
    exit(4);
}

static size_t fwrite_orDie(const void* buffer, size_t sizeToWrite, FILE* file)
{
    size_t const writtenSize = fwrite(buffer, 1, sizeToWrite, file);
    if (writtenSize == sizeToWrite) return sizeToWrite;   /* good */
    /* error */
    perror("fwrite");
    exit(5);
}

static size_t fclose_orDie(FILE* file)
{
    if (!fclose(file)) return 0;
    /* error */
    perror("fclose");
    exit(6);
}


static void compressFile_orDie(const char* fname, const char* outName, int cLevel)
{
    FILE* const fin  = fopen_orDie(fname, "rb");
    FILE* const fout = fopen_orDie(outName, "wb");
    size_t const buffInSize = ZSTD_CStreamInSize();    /* can always read one full block */
    void*  const buffIn  = malloc_orDie(buffInSize);
    size_t const buffOutSize = ZSTD_CStreamOutSize();  /* can always flush a full block */
    void*  const buffOut = malloc_orDie(buffOutSize);

    ZSTD_CStream* const cstream = ZSTD_createCStream();
    if (cstream==NULL) { fprintf(stderr, "ZSTD_createCStream() error \n"); exit(10); }
    size_t const initResult = ZSTD_initCStream(cstream, cLevel);
    if (ZSTD_isError(initResult)) {
        fprintf(stderr, "ZSTD_initCStream() error : %s \n",
                    ZSTD_getErrorName(initResult));
        exit(11);
    }

    size_t read, toRead = buffInSize;
    while( (read = fread_orDie(buffIn, toRead, fin)) ) {
        ZSTD_inBuffer input = { buffIn, read, 0 };
        while (input.pos < input.size) {
            ZSTD_outBuffer output = { buffOut, buffOutSize, 0 };
            toRead = ZSTD_compressStream(cstream, &output , &input);   /* toRead is guaranteed to be <= ZSTD_CStreamInSize() */
            if (ZSTD_isError(toRead)) {
                fprintf(stderr, "ZSTD_compressStream() error : %s \n",
                                ZSTD_getErrorName(toRead));
                exit(12);
            }
            if (toRead > buffInSize) toRead = buffInSize;   /* Safely handle case when `buffInSize` is manually changed to a value < ZSTD_CStreamInSize()*/
            fwrite_orDie(buffOut, output.pos, fout);
        }
    }

    ZSTD_outBuffer output = { buffOut, buffOutSize, 0 };
    size_t const remainingToFlush = ZSTD_endStream(cstream, &output);   /* close frame */
    if (remainingToFlush) { fprintf(stderr, "not fully flushed"); exit(13); }
    fwrite_orDie(buffOut, output.pos, fout);

    ZSTD_freeCStream(cstream);
    fclose_orDie(fout);
    fclose_orDie(fin);
    free(buffIn);
    free(buffOut);
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

    if (argc!=2) {
        printf("wrong arguments\n");
        printf("usage:\n");
        printf("%s FILE\n", exeName);
        return 1;
    }

    const char* const inFilename = argv[1];

    char* const outFilename = createOutFilename_orDie(inFilename);
    compressFile_orDie(inFilename, outFilename, 1);

    free(outFilename);   /* not strictly required, since program execution stops there,
                          * but some static analyzer main complain otherwise */
    return 0;
}
