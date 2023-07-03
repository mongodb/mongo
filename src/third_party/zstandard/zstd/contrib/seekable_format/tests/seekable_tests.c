#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>  // malloc
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "../zstd_seekable.h"


/* ZSTD_seekable_customFile implementation that reads/seeks a buffer while keeping track of total bytes read */
typedef struct {
    const void *ptr;
    size_t size;
    size_t pos;
    size_t totalRead;
} buffWrapperWithTotal_t;

static int readBuffWithTotal(void* opaque, void* buffer, size_t n)
{
    buffWrapperWithTotal_t* const buff = (buffWrapperWithTotal_t*)opaque;
    assert(buff != NULL);
    if (buff->pos + n > buff->size) return -1;
    memcpy(buffer, (const char*)buff->ptr + buff->pos, n);
    buff->pos += n;
    buff->totalRead += n;
    return 0;
}

static int seekBuffWithTotal(void* opaque, long long offset, int origin)
{
    buffWrapperWithTotal_t* const buff = (buffWrapperWithTotal_t*) opaque;
    unsigned long long newOffset;
    assert(buff != NULL);
    switch (origin) {
    case SEEK_SET:
        assert(offset >= 0);
        newOffset = (unsigned long long)offset;
        break;
    case SEEK_CUR:
        newOffset = (unsigned long long)((long long)buff->pos + offset);
        break;
    case SEEK_END:
        newOffset = (unsigned long long)((long long)buff->size + offset);
        break;
    default:
        assert(0);  /* not possible */
    }
    if (newOffset > buff->size) {
        return -1;
    }
    buff->pos = newOffset;
    return 0;
}

/* Basic unit tests for zstd seekable format */
int main(int argc, const char** argv)
{
    unsigned testNb = 1;
    (void)argc; (void)argv;
    printf("Beginning zstd seekable format tests...\n");

    printf("Test %u - simple round trip: ", testNb++);
    {   size_t const inSize = 4000;
        void* const inBuffer = malloc(inSize);
        assert(inBuffer != NULL);

        size_t const seekCapacity = 5000;
        void* const seekBuffer = malloc(seekCapacity);
        assert(seekBuffer != NULL);
        size_t seekSize;

        size_t const outCapacity = inSize;
        void* const outBuffer = malloc(outCapacity);
        assert(outBuffer != NULL);

        ZSTD_seekable_CStream* const zscs = ZSTD_seekable_createCStream();
        assert(zscs != NULL);

        { size_t const initStatus = ZSTD_seekable_initCStream(zscs, 9, 0 /* checksumFlag */, (unsigned)inSize /* maxFrameSize */);
          assert(!ZSTD_isError(initStatus));
        }

        {   ZSTD_outBuffer outb = { .dst=seekBuffer, .pos=0, .size=seekCapacity };
            ZSTD_inBuffer inb = { .src=inBuffer, .pos=0, .size=inSize };

            size_t const cStatus = ZSTD_seekable_compressStream(zscs, &outb, &inb);
            assert(!ZSTD_isError(cStatus));
            assert(inb.pos == inb.size);

            size_t const endStatus = ZSTD_seekable_endStream(zscs, &outb);
            assert(!ZSTD_isError(endStatus));
            seekSize = outb.pos;
        }

        ZSTD_seekable* const stream = ZSTD_seekable_create();
        assert(stream != NULL);
        { size_t const initStatus = ZSTD_seekable_initBuff(stream, seekBuffer, seekSize);
          assert(!ZSTD_isError(initStatus)); }

        { size_t const decStatus = ZSTD_seekable_decompress(stream, outBuffer, outCapacity, 0);
          assert(decStatus == inSize); }

        /* unit test ZSTD_seekTable functions */
        ZSTD_seekTable* const zst = ZSTD_seekTable_create_fromSeekable(stream);
        assert(zst != NULL);

        unsigned const nbFrames = ZSTD_seekTable_getNumFrames(zst);
        assert(nbFrames > 0);

        unsigned long long const frame0Offset = ZSTD_seekTable_getFrameCompressedOffset(zst, 0);
        assert(frame0Offset == 0);

        unsigned long long const content0Offset = ZSTD_seekTable_getFrameDecompressedOffset(zst, 0);
        assert(content0Offset == 0);

        size_t const cSize = ZSTD_seekTable_getFrameCompressedSize(zst, 0);
        assert(!ZSTD_isError(cSize));
        assert(cSize <= seekCapacity);

        size_t const origSize = ZSTD_seekTable_getFrameDecompressedSize(zst, 0);
        assert(origSize == inSize);

        unsigned const fo1idx = ZSTD_seekTable_offsetToFrameIndex(zst, 1);
        assert(fo1idx == 0);

        free(inBuffer);
        free(seekBuffer);
        free(outBuffer);
        ZSTD_seekable_freeCStream(zscs);
        ZSTD_seekTable_free(zst);
        ZSTD_seekable_free(stream);
    }
    printf("Success!\n");


    printf("Test %u - check that seekable decompress does not hang: ", testNb++);
    {   /* Github issue #2335 */
        const size_t compressed_size = 17;
        const uint8_t compressed_data[17] = {
            '^',
            '*',
            'M',
            '\x18',
            '\t',
            '\x00',
            '\x00',
            '\x00',
            '\x00',
            '\x00',
            '\x00',
            '\x00',
            (uint8_t)('\x03'),
            (uint8_t)('\xb1'),
            (uint8_t)('\xea'),
            (uint8_t)('\x92'),
            (uint8_t)('\x8f'),
        };
        const size_t uncompressed_size = 32;
        uint8_t uncompressed_data[32];

        ZSTD_seekable* const stream = ZSTD_seekable_create();
        assert(stream != NULL);
        {   size_t const status = ZSTD_seekable_initBuff(stream, compressed_data, compressed_size);
            if (ZSTD_isError(status)) {
                ZSTD_seekable_free(stream);
                goto _test_error;
        }   }

        /* Should return an error, but not hang */
        {   const size_t offset = 2;
            size_t const status = ZSTD_seekable_decompress(stream, uncompressed_data, uncompressed_size, offset);
            if (!ZSTD_isError(status)) {
                ZSTD_seekable_free(stream);
                goto _test_error;
        }   }

        ZSTD_seekable_free(stream);
    }
    printf("Success!\n");

    printf("Test %u - check #2 that seekable decompress does not hang: ", testNb++);
    {   /* Github issue #FIXME */
        const size_t compressed_size = 27;
        const uint8_t compressed_data[27] = {
            (uint8_t)'\x28',
            (uint8_t)'\xb5',
            (uint8_t)'\x2f',
            (uint8_t)'\xfd',
            (uint8_t)'\x00',
            (uint8_t)'\x32',
            (uint8_t)'\x91',
            (uint8_t)'\x00',
            (uint8_t)'\x00',
            (uint8_t)'\x00',
            (uint8_t)'\x5e',
            (uint8_t)'\x2a',
            (uint8_t)'\x4d',
            (uint8_t)'\x18',
            (uint8_t)'\x09',
            (uint8_t)'\x00',
            (uint8_t)'\x00',
            (uint8_t)'\x00',
            (uint8_t)'\x00',
            (uint8_t)'\x00',
            (uint8_t)'\x00',
            (uint8_t)'\x00',
            (uint8_t)'\x00',
            (uint8_t)'\xb1',
            (uint8_t)'\xea',
            (uint8_t)'\x92',
            (uint8_t)'\x8f',
        };
        const size_t uncompressed_size = 400;
        uint8_t uncompressed_data[400];

        ZSTD_seekable* stream = ZSTD_seekable_create();
        size_t status = ZSTD_seekable_initBuff(stream, compressed_data, compressed_size);
        if (ZSTD_isError(status)) {
            ZSTD_seekable_free(stream);
            goto _test_error;
        }

        const size_t offset = 2;
        /* Should return an error, but not hang */
        status = ZSTD_seekable_decompress(stream, uncompressed_data, uncompressed_size, offset);
        if (!ZSTD_isError(status)) {
            ZSTD_seekable_free(stream);
            goto _test_error;
        }

        ZSTD_seekable_free(stream);
    }
    printf("Success!\n");


    printf("Test %u - check ZSTD magic in compressing empty string: ", testNb++);
    { // compressing empty string should return a zstd header
        size_t const capacity = 255;
        char* inBuffer = malloc(capacity);
        assert(inBuffer != NULL);
        inBuffer[0] = '\0';
        void* const outBuffer = malloc(capacity);
        assert(outBuffer != NULL);

        ZSTD_seekable_CStream *s = ZSTD_seekable_createCStream();
        ZSTD_seekable_initCStream(s, 1, 1, 255);

        ZSTD_inBuffer input = { .src=inBuffer, .pos=0, .size=0 };
        ZSTD_outBuffer output = { .dst=outBuffer, .pos=0, .size=capacity };

        ZSTD_seekable_compressStream(s, &output, &input);
        ZSTD_seekable_endStream(s, &output);

        if((((char*)output.dst)[0] != '\x28') | (((char*)output.dst)[1] != '\xb5') | (((char*)output.dst)[2] != '\x2f') | (((char*)output.dst)[3] != '\xfd')) {
            printf("%#02x %#02x %#02x %#02x\n", ((char*)output.dst)[0], ((char*)output.dst)[1] , ((char*)output.dst)[2] , ((char*)output.dst)[3] );

            free(inBuffer);
            free(outBuffer);
            ZSTD_seekable_freeCStream(s);
            goto _test_error;
        }

        free(inBuffer);
        free(outBuffer);
        ZSTD_seekable_freeCStream(s);
    }
    printf("Success!\n");


    printf("Test %u - multiple decompress calls: ", testNb++);
    {   char const inBuffer[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt";
        size_t const inSize = sizeof(inBuffer);

        size_t const seekCapacity = 5000;
        void* const seekBuffer = malloc(seekCapacity);
        assert(seekBuffer != NULL);
        size_t seekSize;

        size_t const outCapacity = inSize;
        char* const outBuffer = malloc(outCapacity);
        assert(outBuffer != NULL);

        ZSTD_seekable_CStream* const zscs = ZSTD_seekable_createCStream();
        assert(zscs != NULL);

        /* compress test data with a small frame size to ensure multiple frames in the output */
        unsigned const maxFrameSize = 40;
        { size_t const initStatus = ZSTD_seekable_initCStream(zscs, 9, 0 /* checksumFlag */, maxFrameSize);
          assert(!ZSTD_isError(initStatus));
        }

        {   ZSTD_outBuffer outb = { .dst=seekBuffer, .pos=0, .size=seekCapacity };
            ZSTD_inBuffer inb = { .src=inBuffer, .pos=0, .size=inSize };

            while (inb.pos < inb.size) {
                size_t const cStatus = ZSTD_seekable_compressStream(zscs, &outb, &inb);
                assert(!ZSTD_isError(cStatus));
            }

            size_t const endStatus = ZSTD_seekable_endStream(zscs, &outb);
            assert(!ZSTD_isError(endStatus));
            seekSize = outb.pos;
        }

        ZSTD_seekable* const stream = ZSTD_seekable_create();
        assert(stream != NULL);
        buffWrapperWithTotal_t buffWrapper = {seekBuffer, seekSize, 0, 0};
        { ZSTD_seekable_customFile srcFile = {&buffWrapper, &readBuffWithTotal, &seekBuffWithTotal};
          size_t const initStatus = ZSTD_seekable_initAdvanced(stream, srcFile);
          assert(!ZSTD_isError(initStatus)); }

        /* Perform a series of small reads and seeks (repeatedly read 1 byte and skip 1 byte)
           and check that we didn't reread input data unnecessarily */
        size_t pos;
        for (pos = 0; pos < inSize; pos += 2) {
            size_t const decStatus = ZSTD_seekable_decompress(stream, outBuffer, 1, pos);
            if (decStatus != 1 || outBuffer[0] != inBuffer[pos]) {
                goto _test_error;
            }
        }
        if (buffWrapper.totalRead > seekSize) {
            /* We read more than the compressed size, meaning there were some rereads.
               This is unneeded because we only seeked forward. */
            printf("Too much data read: %zu read, with compressed size %zu\n", buffWrapper.totalRead, seekSize);
            goto _test_error;
        }

        /* Perform some reads and seeks to ensure correctness */
        struct {
            size_t offset;
            size_t size;
        } const tests[] = {  /* Assume the frame size is 40 */
            {20, 40}, /* read partial data from two frames */
            {60, 10}, /* continue reading from the same offset */
            {50, 20}, /* seek backward within the same frame */
            {10, 10}, /* seek backward to a different frame */
            {25, 10}, /* seek forward within the same frame */
            {60, 10}, /* seek forward to a different frame */
        };
        size_t idx;
        for (idx = 0; idx < sizeof(tests) / sizeof(tests[0]); idx++) {
            size_t const decStatus = ZSTD_seekable_decompress(stream, outBuffer, tests[idx].size, tests[idx].offset);
            if (decStatus != tests[idx].size || memcmp(outBuffer, inBuffer + tests[idx].offset, tests[idx].size) != 0) {
                goto _test_error;
            }
        }

        free(seekBuffer);
        free(outBuffer);
        ZSTD_seekable_freeCStream(zscs);
        ZSTD_seekable_free(stream);
    }
    printf("Success!\n");

    /* TODO: Add more tests */
    printf("Finished tests\n");
    return 0;

_test_error:
    printf("test failed! Exiting..\n");
    return 1;
}
