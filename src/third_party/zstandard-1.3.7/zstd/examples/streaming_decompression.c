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
#include <stdio.h>     // fprintf, perror, feof
#include <string.h>    // strerror
#include <errno.h>     // errno
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


static void decompressFile_orDie(const char* fname)
{
    FILE* const fin  = fopen_orDie(fname, "rb");
    size_t const buffInSize = ZSTD_DStreamInSize();
    void*  const buffIn  = malloc_orDie(buffInSize);
    FILE* const fout = stdout;
    size_t const buffOutSize = ZSTD_DStreamOutSize();  /* Guarantee to successfully flush at least one complete compressed block in all circumstances. */
    void*  const buffOut = malloc_orDie(buffOutSize);

    ZSTD_DStream* const dstream = ZSTD_createDStream();
    if (dstream==NULL) { fprintf(stderr, "ZSTD_createDStream() error \n"); exit(10); }

    /* In more complex scenarios, a file may consist of multiple appended frames (ex : pzstd).
    *  The following example decompresses only the first frame.
    *  It is compatible with other provided streaming examples */
    size_t const initResult = ZSTD_initDStream(dstream);
    if (ZSTD_isError(initResult)) { fprintf(stderr, "ZSTD_initDStream() error : %s \n", ZSTD_getErrorName(initResult)); exit(11); }
    size_t read, toRead = initResult;
    while ( (read = fread_orDie(buffIn, toRead, fin)) ) {
        ZSTD_inBuffer input = { buffIn, read, 0 };
        while (input.pos < input.size) {
            ZSTD_outBuffer output = { buffOut, buffOutSize, 0 };
            toRead = ZSTD_decompressStream(dstream, &output , &input);  /* toRead : size of next compressed block */
            if (ZSTD_isError(toRead)) { fprintf(stderr, "ZSTD_decompressStream() error : %s \n", ZSTD_getErrorName(toRead)); exit(12); }
            fwrite_orDie(buffOut, output.pos, fout);
        }
    }

    ZSTD_freeDStream(dstream);
    fclose_orDie(fin);
    fclose_orDie(fout);
    free(buffIn);
    free(buffOut);
}


int main(int argc, const char** argv)
{
    const char* const exeName = argv[0];

    if (argc!=2) {
        fprintf(stderr, "wrong arguments\n");
        fprintf(stderr, "usage:\n");
        fprintf(stderr, "%s FILE\n", exeName);
        return 1;
    }

    const char* const inFilename = argv[1];

    decompressFile_orDie(inFilename);
    return 0;
}
