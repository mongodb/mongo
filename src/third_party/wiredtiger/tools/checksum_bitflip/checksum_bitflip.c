/*-
 * Public Domain 2024-present MongoDB, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <test_util.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * This program takes a file and a checksum and checks whether the checksum
 * matches the contents of the file, or if any checksum produced by flipping
 * a single bit of the file contents matches.
 *
 * This is intended for use when debugging WT checksum mismatches when we suspect
 * that the issue is the result of faulty hardware causing bit flips in memory.
 *
 * Usage:
 *     checksum_bitflip cksum filename
 *
 * The checksum argument should be a hex string. The file should contain the data
 * to be tested. Typical usage is to generate this data from the block dump printed
 * when WiredTiger reports a checksum mismatch, for example by copying the hex
 * strings into a file and running "xxd -p -r" on it.
 */

int
main(int argc, char *argv[])
{
    struct stat statbuf;
    int64_t tmp;
    uint32_t cksum_target;
    ssize_t io_bytes;
    size_t byte, size;
    int bit, fd, rval;
    char *filename;
    uint8_t *buffer;
    uint8_t mask;

    if (argc < 3) {
        printf("Usage: %s checksum filename\n", argv[0]);
        return (1);
    }

    tmp = strtol(argv[1], NULL, 16);
    if ((tmp ^ (tmp & 0xFFFFFFFF)) != 0) {
        fprintf(stderr, "Target checksum must be 32-bits\n");
        return (1);
    }
    cksum_target = (uint32_t)tmp;

    filename = argv[2];
    fd = open(filename, O_RDONLY);
    testutil_assert(fd != -1);
    rval = fstat(fd, &statbuf);
    testutil_assert(rval != -1);
    size = (size_t)statbuf.st_size;
    buffer = dmalloc(size);

    io_bytes = read(fd, buffer, size);
    testutil_assert(io_bytes == (ssize_t)size);

    /* See if the checksum matches the file contents. */
    if (__wt_checksum_sw(buffer, size) == cksum_target) {
        printf("Checksum matches without flipping bits\n");
        return (0);
    }

    /*
     * Iterate through the file contents flipping individual bits and checking whether the resulting
     * data generates a matching checksum.
     */
    for (byte = 0; byte < size; byte++) {
        mask = 0x01;
        for (bit = 0; bit < 8; bit++) {
            buffer[byte] ^= mask;
            if (__wt_checksum_sw(buffer, size) == cksum_target) {
                printf("Checksum matches when flipping bit %" PRIi32 " of byte %" WT_SIZET_FMT "\n",
                  bit, byte);
                return (0);
            }
            buffer[byte] ^= mask;
            mask <<= 1;
        }
    }
    printf("No checksum match\n");
    return (1);
}
