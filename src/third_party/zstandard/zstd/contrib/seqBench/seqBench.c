#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

int main(int argc, char *argv[]) {
    ZSTD_CCtx* zc = ZSTD_createCCtx();

    if (argc != 2) {
        printf("Usage: seqBench <file>\n"); // TODO provide the block delim option here
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END);
    long inBufSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *inBuf = malloc(inBufSize + 1);
    fread(inBuf, inBufSize, 1, f);
    fclose(f);

    size_t seqsSize = ZSTD_sequenceBound(inBufSize);
    ZSTD_Sequence *seqs = (ZSTD_Sequence*)malloc(seqsSize * sizeof(ZSTD_Sequence));
    char *outBuf = malloc(ZSTD_compressBound(inBufSize));

    ZSTD_generateSequences(zc, seqs, seqsSize, inBuf, inBufSize);
    ZSTD_CCtx_setParameter(zc, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters);
    size_t outBufSize = ZSTD_compressSequences(zc, outBuf, inBufSize, seqs, seqsSize, inBuf, inBufSize);
    if (ZSTD_isError(outBufSize)) {
        printf("ERROR: %lu\n", outBufSize);
        return 1;
    }

    char *validationBuf = malloc(inBufSize);
    ZSTD_decompress(validationBuf, inBufSize, outBuf, outBufSize);

    if (memcmp(inBuf, validationBuf, inBufSize) == 0) {
        printf("Compression and decompression were successful!\n");
    } else {
        printf("ERROR: input and validation buffers don't match!\n");
        for (int i = 0; i < inBufSize; i++) {
            if (inBuf[i] != validationBuf[i]) {
                printf("First bad index: %d\n", i);
                break;
            }
        }
    }

    return 0;
}
