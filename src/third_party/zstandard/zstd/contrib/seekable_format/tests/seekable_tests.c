#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>  // malloc
#include <stdio.h>
#include <assert.h>

#include "zstd_seekable.h"

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

    /* TODO: Add more tests */
    printf("Finished tests\n");
    return 0;

_test_error:
    printf("test failed! Exiting..\n");
    return 1;
}
