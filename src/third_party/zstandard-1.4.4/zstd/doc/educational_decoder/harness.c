/*
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
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

static size_t read_file(const char *path, u8 **ptr)
{
    FILE* const f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "failed to open file %s \n", path);
        exit(1);
    }

    fseek(f, 0L, SEEK_END);
    size_t const size = (size_t)ftell(f);
    rewind(f);

    *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "failed to allocate memory to hold %s \n", path);
        exit(1);
    }

    size_t const read = fread(*ptr, 1, size, f);
    if (read != size) {  /* must read everything in one pass */
        fprintf(stderr, "error while reading file %s \n", path);
        exit(1);
    }

    fclose(f);

    return read;
}

static void write_file(const char *path, const u8 *ptr, size_t size)
{
    FILE* const f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "failed to open file %s \n", path);
        exit(1);
    }

    size_t written = 0;
    while (written < size) {
        written += fwrite(ptr+written, 1, size, f);
        if (ferror(f)) {
            fprintf(stderr, "error while writing file %s\n", path);
            exit(1);
    }   }

    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.zst> <out_path> [dictionary] \n",
                argv[0]);

        return 1;
    }

    u8* input;
    size_t const input_size = read_file(argv[1], &input);

    u8* dict = NULL;
    size_t dict_size = 0;
    if (argc >= 4) {
        dict_size = read_file(argv[3], &dict);
    }

    size_t out_capacity = ZSTD_get_decompressed_size(input, input_size);
    if (out_capacity == (size_t)-1) {
        out_capacity = MAX_COMPRESSION_RATIO * input_size;
        fprintf(stderr, "WARNING: Compressed data does not contain "
                        "decompressed size, going to assume the compression "
                        "ratio is at most %d (decompressed size of at most "
                        "%u) \n",
                MAX_COMPRESSION_RATIO, (unsigned)out_capacity);
    }
    if (out_capacity > MAX_OUTPUT_SIZE) {
        fprintf(stderr,
                "Required output size too large for this implementation \n");
        return 1;
    }

    u8* const output = malloc(out_capacity);
    if (!output) {
        fprintf(stderr, "failed to allocate memory \n");
        return 1;
    }

    dictionary_t* const parsed_dict = create_dictionary();
    if (dict) {
        parse_dictionary(parsed_dict, dict, dict_size);
    }
    size_t const decompressed_size =
        ZSTD_decompress_with_dict(output, out_capacity,
                                  input, input_size,
                                  parsed_dict);

    free_dictionary(parsed_dict);

    write_file(argv[2], output, decompressed_size);

    free(input);
    free(output);
    free(dict);
    return 0;
}
