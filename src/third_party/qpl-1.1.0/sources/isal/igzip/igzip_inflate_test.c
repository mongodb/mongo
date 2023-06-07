/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
#include "igzip_lib.h"
#include "huff_codes.h"
#include "test.h"

/*Don't use file larger memory can support because compression and decompression
 * are done in a stateless manner. */
#if __WORDSIZE == 64
#define MAX_INPUT_FILE_SIZE 2L*1024L*1024L*1024L
#else
#define MAX_INPUT_FILE_SIZE 512L*1024L*1024L
#endif

int inflate_multi_pass(uint8_t * compress_buf, uint64_t compress_len,
		       uint8_t * uncompress_buf, uint32_t * uncompress_len)
{
	struct inflate_state *state = NULL;
	int ret = 0;
	uint8_t *comp_tmp = NULL, *uncomp_tmp = NULL;
	uint32_t comp_tmp_size = 0, uncomp_tmp_size = 0;
	uint32_t comp_processed = 0, uncomp_processed = 0;

	state = malloc(sizeof(struct inflate_state));
	if (state == NULL) {
		printf("Failed to allocate memory\n");
		exit(0);
	}

	isal_inflate_init(state);

	state->next_in = NULL;
	state->next_out = NULL;
	state->avail_in = 0;
	state->avail_out = 0;

	while (1) {
		if (state->avail_in == 0) {
			comp_tmp_size = rand() % (compress_len + 1);

			if (comp_tmp_size >= compress_len - comp_processed)
				comp_tmp_size = compress_len - comp_processed;

			if (comp_tmp_size != 0) {
				if (comp_tmp != NULL) {
					free(comp_tmp);
					comp_tmp = NULL;
				}

				comp_tmp = malloc(comp_tmp_size);

				if (comp_tmp == NULL) {
					printf("Failed to allocate memory\n");
					exit(0);
				}

				memcpy(comp_tmp, compress_buf + comp_processed, comp_tmp_size);
				comp_processed += comp_tmp_size;

				state->next_in = comp_tmp;
				state->avail_in = comp_tmp_size;
			}
		}

		if (state->avail_out == 0) {
			/* Save uncompressed data into uncompress_buf */
			if (uncomp_tmp != NULL) {
				memcpy(uncompress_buf + uncomp_processed, uncomp_tmp,
				       uncomp_tmp_size);
				uncomp_processed += uncomp_tmp_size;
			}

			uncomp_tmp_size = rand() % (*uncompress_len + 1);

			/* Limit size of buffer to be smaller than maximum */
			if (uncomp_tmp_size > *uncompress_len - uncomp_processed)
				uncomp_tmp_size = *uncompress_len - uncomp_processed;

			if (uncomp_tmp_size != 0) {

				if (uncomp_tmp != NULL) {
					fflush(0);
					free(uncomp_tmp);
					uncomp_tmp = NULL;
				}

				uncomp_tmp = malloc(uncomp_tmp_size);
				if (uncomp_tmp == NULL) {
					printf("Failed to allocate memory\n");
					exit(0);
				}

				state->avail_out = uncomp_tmp_size;
				state->next_out = uncomp_tmp;
			}
		}

		ret = isal_inflate(state);

		if (state->block_state == ISAL_BLOCK_FINISH || ret != 0) {
			memcpy(uncompress_buf + uncomp_processed, uncomp_tmp, uncomp_tmp_size);
			*uncompress_len = state->total_out;
			break;
		}
	}

	if (comp_tmp != NULL) {
		free(comp_tmp);
		comp_tmp = NULL;
	}
	if (uncomp_tmp != NULL) {
		free(uncomp_tmp);
		uncomp_tmp = NULL;
	}

	free(state);
	return ret;
}

int test(uint8_t * compressed_stream,
	 uint64_t * compressed_length,
	 uint8_t * uncompressed_stream, uint32_t uncompressed_length,
	 uint8_t * uncompressed_test_stream, uint32_t uncompressed_test_stream_length)
{
	int ret;
	ret =
	    compress2(compressed_stream, (uLongf *) compressed_length,
		      uncompressed_stream, uncompressed_length, 6);
	if (ret) {
		printf("Failed compressing input with exit code %d", ret);
		return ret;
	}

	ret =
	    inflate_multi_pass(compressed_stream + 2,
			       *compressed_length - 2 - 4,
			       uncompressed_test_stream, &uncompressed_test_stream_length);
	switch (ret) {
	case 0:
		break;
	case ISAL_END_INPUT:
		printf(" did not decompress all input\n");
		return ISAL_END_INPUT;
		break;
	case ISAL_INVALID_BLOCK:
		printf("  invalid header\n");
		return ISAL_INVALID_BLOCK;
		break;
	case ISAL_INVALID_SYMBOL:
		printf(" invalid symbol\n");
		return ISAL_INVALID_SYMBOL;
		break;
	case ISAL_OUT_OVERFLOW:
		printf(" out buffer overflow\n");
		return ISAL_OUT_OVERFLOW;
		break;
	case ISAL_INVALID_LOOKBACK:
		printf("Invalid lookback distance");
		return ISAL_INVALID_LOOKBACK;
		break;
	default:
		printf(" error\n");
		return -1;
		break;
	}

	if (uncompressed_test_stream_length != uncompressed_length) {
		printf("incorrect amount of data was decompressed from compressed data\n");
		printf("%d decompressed of %d compressed",
		       uncompressed_test_stream_length, uncompressed_length);
		return -1;
	}
	if (memcmp(uncompressed_stream, uncompressed_test_stream, uncompressed_length)) {
		int i;
		for (i = 0; i < uncompressed_length; i++) {
			if (uncompressed_stream[i] != uncompressed_test_stream[i]) {
				printf("first error at %d, 0x%x != 0x%x\n", i,
				       uncompressed_stream[i], uncompressed_test_stream[i]);
			}
		}
		printf(" decompressed data is not the same as the compressed data\n");
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int i, j, ret = 0, fin_ret = 0;
	FILE *file = NULL;
	uint64_t compressed_length, file_length;
	uint64_t uncompressed_length, uncompressed_test_stream_length;
	uint8_t *uncompressed_stream = NULL;
	uint8_t *compressed_stream = NULL;
	uint8_t *uncompressed_test_stream = NULL;

	if (argc == 1)
		printf("Error, no input file\n");
	for (i = 1; i < argc; i++) {

		file = NULL;
		uncompressed_stream = NULL;
		compressed_stream = NULL;
		uncompressed_test_stream = NULL;

		file = fopen(argv[i], "r");
		if (file == NULL) {
			printf("Error opening file %s\n", argv[i]);
			return 1;
		} else
			printf("Starting file %s", argv[i]);
		fflush(0);
		file_length = get_filesize(file);
		if (file_length > MAX_INPUT_FILE_SIZE) {
			printf("\nFile too large to run on this test,"
			       " Max 512MB for 32bit OS, 2GB for 64bit OS.\n");
			printf(" ... Fail\n");
			fclose(file);
			continue;
		}

		compressed_length = compressBound(file_length);

		if (file_length != 0) {
			uncompressed_stream = malloc(file_length);
			uncompressed_test_stream = malloc(file_length);
		}

		compressed_stream = malloc(compressed_length);
		if (uncompressed_stream == NULL && file_length != 0) {
			printf("\nFailed to allocate input memory\n");
			exit(0);
		}

		if (compressed_stream == NULL) {
			printf("\nFailed to allocate output memory\n");
			exit(0);
		}

		if (uncompressed_test_stream == NULL && file_length != 0) {
			printf("\nFailed to allocate decompressed memory\n");
			exit(0);
		}

		uncompressed_length = fread(uncompressed_stream, 1, file_length, file);
		uncompressed_test_stream_length = uncompressed_length;
		ret =
		    test(compressed_stream, &compressed_length, uncompressed_stream,
			 uncompressed_length, uncompressed_test_stream,
			 uncompressed_test_stream_length);
		if (ret) {
			for (j = 0; j < compressed_length; j++) {
				if ((j & 31) == 0)
					printf("\n");
				else
					printf(" ");
				printf("0x%02x,", compressed_stream[j]);
			}
			printf("\n");
		}

		fflush(0);
		fclose(file);
		if (compressed_stream != NULL)
			free(compressed_stream);
		if (uncompressed_stream != NULL)
			free(uncompressed_stream);
		if (uncompressed_test_stream != NULL)
			free(uncompressed_test_stream);
		if (ret) {
			printf(" ... Fail with exit code %d\n", ret);
			return ret;
		} else
			printf(" ... Pass\n");
		fin_ret |= ret;
	}
	return fin_ret;
}
