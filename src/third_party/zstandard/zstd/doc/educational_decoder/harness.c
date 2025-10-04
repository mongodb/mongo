/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include <stdio.h>
#include <stdlib.h>

#include "zstd_decompress.h"

typedef unsigned char u8;

// If the data doesn't have decompressed size with it, fallback on assuming the
// compression ratio is at most 16
#define MAX_COMPRESSION_RATIO (16)

// Protect against allocating too much memory for output
#define MAX_OUTPUT_SIZE ((size_t)1024 * 1024 * 1024)

// Error message then exit
#define ERR_OUT(...) { fprintf(stderr, __VA_ARGS__); exit(1); }


typedef struct {
    u8* address;
    size_t size;
} buffer_s;

static void freeBuffer(buffer_s b) { free(b.address); }

static buffer_s read_file(const char *path)
{
    FILE* const f = fopen(path, "rb");
    if (!f) ERR_OUT("failed to open file %s \n", path);

    fseek(f, 0L, SEEK_END);
    size_t const size = (size_t)ftell(f);
    rewind(f);

    void* const ptr = malloc(size);
    if (!ptr) ERR_OUT("failed to allocate memory to hold %s \n", path);

    size_t const read = fread(ptr, 1, size, f);
    if (read != size) ERR_OUT("error while reading file %s \n", path);

    fclose(f);
    buffer_s const b = { ptr, size };
    return b;
}

static void write_file(const char* path, const u8* ptr, size_t size)
{
    FILE* const f = fopen(path, "wb");
    if (!f) ERR_OUT("failed to open file %s \n", path);

    size_t written = 0;
    while (written < size) {
        written += fwrite(ptr+written, 1, size, f);
        if (ferror(f)) ERR_OUT("error while writing file %s\n", path);
    }

    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        ERR_OUT("usage: %s <file.zst> <out_path> [dictionary] \n", argv[0]);

    buffer_s const input = read_file(argv[1]);

    buffer_s dict = { NULL, 0 };
    if (argc >= 4) {
        dict = read_file(argv[3]);
    }

    size_t out_capacity = ZSTD_get_decompressed_size(input.address, input.size);
    if (out_capacity == (size_t)-1) {
        out_capacity = MAX_COMPRESSION_RATIO * input.size;
        fprintf(stderr, "WARNING: Compressed data does not contain "
                        "decompressed size, going to assume the compression "
                        "ratio is at most %d (decompressed size of at most "
                        "%u) \n",
                MAX_COMPRESSION_RATIO, (unsigned)out_capacity);
    }
    if (out_capacity > MAX_OUTPUT_SIZE)
        ERR_OUT("Required output size too large for this implementation \n");

    u8* const output = malloc(out_capacity);
    if (!output) ERR_OUT("failed to allocate memory \n");

    dictionary_t* const parsed_dict = create_dictionary();
    if (dict.size) {
#if defined (ZDEC_NO_DICTIONARY)
        printf("dict.size = %zu \n", dict.size);
        ERR_OUT("no dictionary support \n");
#else
        parse_dictionary(parsed_dict, dict.address, dict.size);
#endif
    }
    size_t const decompressed_size =
        ZSTD_decompress_with_dict(output, out_capacity,
                                  input.address, input.size,
                                  parsed_dict);

    free_dictionary(parsed_dict);

    write_file(argv[2], output, decompressed_size);

    freeBuffer(input);
    freeBuffer(dict);
    free(output);
    return 0;
}
