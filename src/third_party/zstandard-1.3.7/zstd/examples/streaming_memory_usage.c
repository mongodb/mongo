/*
 * Copyright (c) 2017-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


/*===   Tuning parameter   ===*/
#ifndef MAX_TESTED_LEVEL
#define MAX_TESTED_LEVEL 12
#endif


/*===   Dependencies   ===*/
#include <stdio.h>   /* printf */
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"


/*===   functions   ===*/

/*! readU32FromChar() :
    @return : unsigned integer value read from input in `char` format
    allows and interprets K, KB, KiB, M, MB and MiB suffix.
    Will also modify `*stringPtr`, advancing it to position where it stopped reading.
    Note : function result can overflow if digit string > MAX_UINT */
static unsigned readU32FromChar(const char** stringPtr)
{
    unsigned result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9'))
        result *= 10, result += **stringPtr - '0', (*stringPtr)++ ;
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        result <<= 10;
        if (**stringPtr=='M') result <<= 10;
        (*stringPtr)++ ;
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    return result;
}


int main(int argc, char const *argv[]) {

    printf("\n Zstandard (v%s) memory usage for streaming : \n\n", ZSTD_versionString());

    unsigned wLog = 0;
    if (argc > 1) {
        const char* valStr = argv[1];
        wLog = readU32FromChar(&valStr);
    }

    int compressionLevel;
    for (compressionLevel = 1; compressionLevel <= MAX_TESTED_LEVEL; compressionLevel++) {
#define INPUT_SIZE 5
#define COMPRESSED_SIZE 128
        char const dataToCompress[INPUT_SIZE] = "abcde";
        char compressedData[COMPRESSED_SIZE];
        char decompressedData[INPUT_SIZE];
        ZSTD_CStream* const cstream = ZSTD_createCStream();
        if (cstream==NULL) {
            printf("Level %i : ZSTD_CStream Memory allocation failure \n", compressionLevel);
            return 1;
        }

        /* forces compressor to use maximum memory size for given compression level,
         * by not providing any information on input size */
        ZSTD_parameters params = ZSTD_getParams(compressionLevel, ZSTD_CONTENTSIZE_UNKNOWN, 0);
        if (wLog) { /* special mode : specific wLog */
            printf("Using custom compression parameter : level 1 + wLog=%u \n", wLog);
            params = ZSTD_getParams(1 /*compressionLevel*/,
                                    1 << wLog /*estimatedSrcSize*/,
                                    0 /*no dictionary*/);
            size_t const error = ZSTD_initCStream_advanced(cstream, NULL, 0, params, ZSTD_CONTENTSIZE_UNKNOWN);
            if (ZSTD_isError(error)) {
                printf("ZSTD_initCStream_advanced error : %s \n", ZSTD_getErrorName(error));
                return 1;
            }
        } else {
            size_t const error = ZSTD_initCStream(cstream, compressionLevel);
            if (ZSTD_isError(error)) {
                printf("ZSTD_initCStream error : %s \n", ZSTD_getErrorName(error));
                return 1;
            }
        }

        size_t compressedSize;
        {   ZSTD_inBuffer inBuff = { dataToCompress, sizeof(dataToCompress), 0 };
            ZSTD_outBuffer outBuff = { compressedData, sizeof(compressedData), 0 };
            size_t const cError = ZSTD_compressStream(cstream, &outBuff, &inBuff);
            if (ZSTD_isError(cError)) {
                printf("ZSTD_compressStream error : %s \n", ZSTD_getErrorName(cError));
                return 1;
            }
            size_t const fError = ZSTD_endStream(cstream, &outBuff);
            if (ZSTD_isError(fError)) {
                printf("ZSTD_endStream error : %s \n", ZSTD_getErrorName(fError));
                return 1;
            }
            compressedSize = outBuff.pos;
        }

        ZSTD_DStream* dstream = ZSTD_createDStream();
        if (dstream==NULL) {
            printf("Level %i : ZSTD_DStream Memory allocation failure \n", compressionLevel);
            return 1;
        }
        {   size_t const error = ZSTD_initDStream(dstream);
            if (ZSTD_isError(error)) {
                printf("ZSTD_initDStream error : %s \n", ZSTD_getErrorName(error));
                return 1;
            }
        }
        /* forces decompressor to use maximum memory size, as decompressed size is not known */
        {   ZSTD_inBuffer inBuff = { compressedData, compressedSize, 0 };
            ZSTD_outBuffer outBuff = { decompressedData, sizeof(decompressedData), 0 };
            size_t const dResult = ZSTD_decompressStream(dstream, &outBuff, &inBuff);
            if (ZSTD_isError(dResult)) {
                printf("ZSTD_decompressStream error : %s \n", ZSTD_getErrorName(dResult));
                return 1;
            }
            if (dResult != 0) {
                printf("ZSTD_decompressStream error : unfinished decompression \n");
                return 1;
            }
            if (outBuff.pos != sizeof(dataToCompress)) {
                printf("ZSTD_decompressStream error : incorrect decompression \n");
                return 1;
            }
        }

        size_t const cstreamSize = ZSTD_sizeof_CStream(cstream);
        size_t const cstreamEstimatedSize = wLog ?
                ZSTD_estimateCStreamSize_usingCParams(params.cParams) :
                ZSTD_estimateCStreamSize(compressionLevel);
        size_t const dstreamSize = ZSTD_sizeof_DStream(dstream);

        printf("Level %2i : Compression Mem = %5u KB (estimated : %5u KB) ; Decompression Mem = %4u KB \n",
                compressionLevel,
                (unsigned)(cstreamSize>>10), (unsigned)(cstreamEstimatedSize>>10), (unsigned)(dstreamSize>>10));

        ZSTD_freeDStream(dstream);
        ZSTD_freeCStream(cstream);
        if (wLog) break;  /* single test */
    }
    return 0;
}
