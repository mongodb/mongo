/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include "igzip_lib.h"
#include "checksum_test_ref.h"
#include "inflate_std_vects.h"
#include <math.h>
#include "test.h"
#include "unaligned.h"

#ifdef HAVE_GETOPT
#include <getopt.h>
#endif

#ifndef RANDOMS
# define RANDOMS   0x40
#endif
#ifndef TEST_SEED
# define TEST_SEED 0x1234
#endif

#define MAX_BITS_COUNT 20
#define MIN_BITS_COUNT 8

#define IBUF_SIZE  (1024*1024)

#define MAX_LARGE_COMP_BUF_SIZE (1024*1024)

#define PAGE_SIZE 4*1024

#define MAX_FILE_SIZE 0x7fff8fff

#define str1 "Short test string"
#define str2 "one two three four five six seven eight nine ten eleven twelve " \
		 "thirteen fourteen fifteen sixteen"

#define TYPE0_HDR_SIZE 5	/* Size of a type 0 blocks header in bytes */
#define TYPE0_MAX_SIZE 65535	/* Max length of a type 0 block in bytes (excludes the header) */

#define MAX_LOOPS 20
/* Defines for the possible error conditions */
enum IGZIP_TEST_ERROR_CODES {
	IGZIP_COMP_OK = 0,

	MALLOC_FAILED,
	FILE_READ_FAILED,

	COMPRESS_INCORRECT_STATE,
	COMPRESS_INPUT_STREAM_INTEGRITY_ERROR,
	COMPRESS_OUTPUT_STREAM_INTEGRITY_ERROR,
	COMPRESS_END_OF_STREAM_NOT_SET,
	COMPRESS_ALL_INPUT_FAIL,
	COMPRESS_OUT_BUFFER_OVERFLOW,
	COMPRESS_LOOP_COUNT_OVERFLOW,
	COMPRESS_GENERAL_ERROR,

	INFLATE_END_OF_INPUT,
	INFLATE_INVALID_BLOCK_HEADER,
	INFLATE_INVALID_SYMBOL,
	INFLATE_OUT_BUFFER_OVERFLOW,
	INFLATE_LEFTOVER_INPUT,
	INFLATE_INCORRECT_OUTPUT_SIZE,
	INFLATE_INVALID_LOOK_BACK_DISTANCE,
	INFLATE_INPUT_STREAM_INTEGRITY_ERROR,
	INFLATE_OUTPUT_STREAM_INTEGRITY_ERROR,
	INVALID_GZIP_HEADER,
	INCORRECT_GZIP_TRAILER,
	INVALID_ZLIB_HEADER,
	INCORRECT_ZLIB_TRAILER,

	UNSUPPORTED_METHOD,

	INFLATE_GENERAL_ERROR,

	INVALID_FLUSH_ERROR,

	OVERFLOW_TEST_ERROR,
	RESULT_ERROR
};

static const int hdr_bytes = 300;

static const uint32_t gzip_trl_bytes = 8;
static const uint32_t zlib_trl_bytes = 4;
static const int gzip_extra_bytes = 18;	/* gzip_hdr_bytes + gzip_trl_bytes */
static const int zlib_extra_bytes = 6;	/* zlib_hdr_bytes + zlib_trl_bytes */

int inflate_type = 0;

struct isal_hufftables *hufftables = NULL;
struct isal_hufftables *hufftables_subset = NULL;

#define HISTORY_SIZE 32*1024
#define MIN_LENGTH 3
#define MIN_DIST 1

struct test_options {
	int test_seed;
	int randoms;
	int do_large_test;
	int verbose;

};

struct test_options options;

void init_options(void)
{
	options.test_seed = TEST_SEED;
	options.randoms = RANDOMS;
	options.do_large_test = 1;
#ifdef VERBOSE
	options.verbose = 1;
#else
	options.verbose = 0;
#endif
}

void usage(void)
{
	fprintf(stderr,
		"Usage: igzip_rand_test [options] [FILES]\n"
		"  -h          help, print this message\n"
		"  -l          turn off large input test\n"
		"  -r <iter>   number of randoms for each test\n"
		"  -s <seed>   set rand() test seed\n"
		"  -v          enable verbose test log\n");
	exit(0);
}

size_t parse_options(int argc, char *argv[])
{
	init_options();
#ifdef HAVE_GETOPT
	int c;
	char optstring[] = "hlr:s:v";
	while ((c = getopt(argc, argv, optstring)) != -1) {
		switch (c) {
		case 'l':
			options.do_large_test = 0;
			break;
		case 'r':
			options.randoms = atoi(optarg);
			break;
		case 's':
			options.test_seed = atoi(optarg);
			break;
		case 'v':
			options.verbose = 1;
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}
	return optind;
#else
	return 1;
#endif
}

/* Create random compressible data. This is achieved by randomly choosing a
 * random character, or to repeat previous data in the stream for a random
 * length and look back distance. The probability of a random character or a
 * repeat being chosen is semi-randomly chosen by setting max_repeat_data to be
 * differing values */
void create_rand_repeat_data(uint8_t * data, int size)
{
	uint32_t next_data;
	uint8_t *data_start = data;
	uint32_t length, distance;
	uint32_t symbol_count = rand() % 255 + 1, swaps_left, tmp;
	uint32_t max_repeat_data = symbol_count;
	uint8_t symbols[256], *symbols_next, swap_val;

	/* An array of the powers of 2 (except the final element which is 0) */
	const uint32_t power_of_2_array[] = {
		0x00000001, 0x00000002, 0x00000004, 0x00000008,
		0x00000010, 0x00000020, 0x00000040, 0x00000080,
		0x00000100, 0x00000200, 0x00000400, 0x00000800,
		0x00001000, 0x00002000, 0x00004000, 0x00008000,
		0x00010000, 0x00020000, 0x00040000, 0x00080000,
		0x00100000, 0x00200000, 0x00400000, 0x00800000,
		0x01000000, 0x02000000, 0x04000000, 0x08000000,
		0x10000000, 0x20000000, 0x40000000, 0x00000000
	};

	uint32_t power = rand() % sizeof(power_of_2_array) / sizeof(uint32_t);

	if (symbol_count > 128) {
		memset(symbols, 1, sizeof(symbols));
		swap_val = 0;
		swaps_left = 256 - symbol_count;
	} else {
		memset(symbols, 0, sizeof(symbols));
		swap_val = 1;
		swaps_left = symbol_count;
	}

	while (swaps_left > 0) {
		tmp = rand() % 256;
		if (symbols[tmp] != swap_val) {
			symbols[tmp] = swap_val;
			swaps_left--;
		}
	}

	symbols_next = symbols;
	for (tmp = 0; tmp < 256; tmp++) {
		if (symbols[tmp]) {
			*symbols_next = tmp;
			symbols_next++;
		}
	}

	max_repeat_data += power_of_2_array[power];

	if (size > 0) {
		size--;
		*data++ = rand();
	}

	while (size > 0) {
		next_data = rand() % max_repeat_data;
		if (next_data < symbol_count) {
			*data++ = symbols[next_data];
			size--;
		} else if (size < 3) {
			*data++ = symbols[rand() % symbol_count];
			size--;
		} else {
			length = (rand() % 256) + MIN_LENGTH;
			if (length > size)
				length = (rand() % (size - 2)) + MIN_LENGTH;

			distance = (rand() % HISTORY_SIZE) + MIN_DIST;
			if (distance > data - data_start)
				distance = (rand() % (data - data_start)) + MIN_DIST;

			size -= length;
			if (distance <= length) {
				while (length-- > 0) {
					*data = *(data - distance);
					data++;
				}
			} else {
				memcpy(data, data - distance, length);
				data += length;
			}
		}
	}
}

void create_rand_dict(uint8_t * dict, uint32_t dict_len, uint8_t * buf, uint32_t buf_len)
{
	uint32_t dict_chunk_size, buf_chunk_size;
	while (dict_len > 0) {
		dict_chunk_size = rand() % IGZIP_K;
		dict_chunk_size = (dict_len >= dict_chunk_size) ? dict_chunk_size : dict_len;

		buf_chunk_size = rand() % IGZIP_K;
		buf_chunk_size = (buf_len >= buf_chunk_size) ? buf_chunk_size : buf_len;

		if (rand() % 3 == 0 && buf_len >= dict_len)
			memcpy(dict, buf, dict_chunk_size);
		else
			create_rand_repeat_data(dict, dict_chunk_size);

		dict_len -= dict_chunk_size;
		dict += dict_chunk_size;
		buf_len -= buf_chunk_size;
		buf += buf_chunk_size;
	}

}

int get_rand_data_length(void)
{
	int max_mask =
	    (1 << ((rand() % (MAX_BITS_COUNT - MIN_BITS_COUNT)) + MIN_BITS_COUNT)) - 1;
	return rand() & max_mask;
}

int get_rand_level(void)
{
	return ISAL_DEF_MIN_LEVEL + rand() % (ISAL_DEF_MAX_LEVEL - ISAL_DEF_MIN_LEVEL + 1);

}

int get_rand_level_buf_size(int level)
{
	int size;
	switch (level) {
	case 3:
		size = rand() % IBUF_SIZE + ISAL_DEF_LVL3_MIN;
		break;
	case 2:
		size = rand() % IBUF_SIZE + ISAL_DEF_LVL2_MIN;
		break;
	case 1:
	default:
		size = rand() % IBUF_SIZE + ISAL_DEF_LVL1_MIN;
	}
	return size;
}

void print_error(int error_code)
{
	switch (error_code) {
	case IGZIP_COMP_OK:
		break;
	case MALLOC_FAILED:
		printf("error: failed to allocate memory\n");
		break;
	case FILE_READ_FAILED:
		printf("error: failed to read in file\n");
		break;
	case COMPRESS_INCORRECT_STATE:
		printf("error: incorrect stream internal state\n");
		break;
	case COMPRESS_INPUT_STREAM_INTEGRITY_ERROR:
		printf("error: inconsistent stream input buffer\n");
		break;
	case COMPRESS_OUTPUT_STREAM_INTEGRITY_ERROR:
		printf("error: inconsistent stream output buffer\n");
		break;
	case COMPRESS_END_OF_STREAM_NOT_SET:
		printf("error: end of stream not set\n");
		break;
	case COMPRESS_ALL_INPUT_FAIL:
		printf("error: not all input data compressed\n");
		break;
	case COMPRESS_OUT_BUFFER_OVERFLOW:
		printf("error: output buffer overflow while compressing data\n");
		break;
	case COMPRESS_GENERAL_ERROR:
		printf("error: compression failed\n");
		break;
	case INFLATE_END_OF_INPUT:
		printf("error: did not decompress all input\n");
		break;
	case INFLATE_INVALID_BLOCK_HEADER:
		printf("error: invalid header\n");
		break;
	case INFLATE_INVALID_SYMBOL:
		printf("error: invalid symbol found when decompressing input\n");
		break;
	case INFLATE_OUT_BUFFER_OVERFLOW:
		printf("error: output buffer overflow while decompressing data\n");
		break;
	case INFLATE_GENERAL_ERROR:
		printf("error: decompression failed\n");
		break;
	case INFLATE_LEFTOVER_INPUT:
		printf("error: the trailer of igzip output contains junk\n");
		break;
	case INFLATE_INCORRECT_OUTPUT_SIZE:
		printf("error: incorrect amount of data was decompressed\n");
		break;
	case INFLATE_INVALID_LOOK_BACK_DISTANCE:
		printf("error: invalid look back distance found while decompressing\n");
		break;
	case INFLATE_INPUT_STREAM_INTEGRITY_ERROR:
		printf("error: inconsistent input buffer\n");
		break;
	case INFLATE_OUTPUT_STREAM_INTEGRITY_ERROR:
		printf("error: inconsistent output buffer\n");
		break;
	case INVALID_GZIP_HEADER:
		printf("error: incorrect gzip header found when inflating data\n");
		break;
	case INCORRECT_GZIP_TRAILER:
		printf("error: incorrect gzip trailer found when inflating data\n");
		break;
	case INVALID_ZLIB_HEADER:
		printf("error: incorrect zlib header found when inflating data\n");
		break;
	case INCORRECT_ZLIB_TRAILER:
		printf("error: incorrect zlib trailer found when inflating data\n");
		break;
	case UNSUPPORTED_METHOD:
		printf("error: invalid compression method in wrapper header\n");
		break;
	case INVALID_FLUSH_ERROR:
		printf("error: invalid flush did not cause compression to error\n");
		break;
	case RESULT_ERROR:
		printf("error: decompressed data is not the same as the compressed data\n");
		break;
	case OVERFLOW_TEST_ERROR:
		printf("error: overflow undetected\n");
		break;
	default:
		printf("error: unknown error code\n");
	}
}

void print_uint8_t(uint8_t * array, uint64_t length)
{
	const int line_size = 16;
	int i;

	printf("Length = %lu", length);
	for (i = 0; i < length; i++) {
		if ((i % line_size) == 0)
			printf("\n0x%08x\t", i);
		else
			printf(" ");
		printf("0x%02x,", array[i]);
	}
	printf("\n");
}

void log_print(char *format, ...)
{
	va_list args;
	va_start(args, format);

	if (options.verbose)
		vfprintf(stdout, format, args);

	va_end(args);
}

void log_uint8_t(uint8_t * array, uint64_t length)
{
	if (options.verbose)
		print_uint8_t(array, length);
}

void log_error(int error_code)
{
	if (options.verbose)
		print_error(error_code);
}

uint32_t check_gzip_trl(uint64_t gzip_trl, uint32_t inflate_crc, uint8_t * uncompress_buf,
			uint32_t uncompress_len)
{
	uint64_t trl, ret = 0;
	uint32_t crc;

	crc = crc32_gzip_refl_ref(0, uncompress_buf, uncompress_len);
	trl = ((uint64_t) uncompress_len << 32) | crc;

	if (crc != inflate_crc || trl != gzip_trl)
		ret = INCORRECT_GZIP_TRAILER;

	return ret;
}

uint32_t check_zlib_trl(uint32_t zlib_trl, uint32_t inflate_adler, uint8_t * uncompress_buf,
			uint32_t uncompress_len)
{
	uint32_t trl, ret = 0;
	uint32_t adler;

	adler = adler_ref(1, uncompress_buf, uncompress_len);

	trl =
	    (adler >> 24) | ((adler >> 8) & 0xFF00) | (adler << 24) | ((adler & 0xFF00) << 8);

	if (adler != inflate_adler || trl != zlib_trl) {
		ret = INCORRECT_ZLIB_TRAILER;
	}

	return ret;
}

int inflate_stateless_pass(uint8_t * compress_buf, uint64_t compress_len,
			   uint8_t * uncompress_buf, uint32_t * uncompress_len,
			   uint32_t gzip_flag)
{
	struct inflate_state state;
	int ret = 0, offset = 0;
	struct isal_gzip_header gz_hdr;
	struct isal_zlib_header z_hdr;

	state.next_in = compress_buf;
	state.avail_in = compress_len;
	state.next_out = uncompress_buf;
	state.avail_out = *uncompress_len;

	if (gzip_flag == IGZIP_GZIP) {
		if (rand() % 2 == 0) {
			memset(&gz_hdr, 0, sizeof(gz_hdr));
			isal_inflate_reset(&state);
			state.tmp_in_size = 0;
			gzip_flag = ISAL_GZIP_NO_HDR_VER;

			isal_read_gzip_header(&state, &gz_hdr);
		}
	} else if (gzip_flag == IGZIP_ZLIB) {
		if (rand() % 2 == 0) {
			memset(&z_hdr, 0, sizeof(z_hdr));
			isal_inflate_reset(&state);
			gzip_flag = ISAL_ZLIB_NO_HDR_VER;
			isal_read_zlib_header(&state, &z_hdr);
		}
	}

	state.crc_flag = gzip_flag;

	ret = isal_inflate_stateless(&state);

	*uncompress_len = state.total_out;

	if (gzip_flag) {
		if (gzip_flag == IGZIP_GZIP || gzip_flag == IGZIP_GZIP_NO_HDR
		    || gzip_flag == ISAL_GZIP_NO_HDR_VER) {
			if (gzip_flag == IGZIP_GZIP || gzip_flag == ISAL_GZIP_NO_HDR_VER)
				offset = gzip_trl_bytes;

			if (!ret)
				ret =
				    check_gzip_trl(load_u64(state.next_in - offset),
						   state.crc, uncompress_buf, *uncompress_len);
			else if (ret == ISAL_INCORRECT_CHECKSUM)
				ret = INCORRECT_GZIP_TRAILER;
			state.avail_in -= (gzip_trl_bytes - offset);
		} else if (gzip_flag == IGZIP_ZLIB || gzip_flag == IGZIP_ZLIB_NO_HDR
			   || gzip_flag == ISAL_ZLIB_NO_HDR_VER) {
			if (gzip_flag == IGZIP_ZLIB || gzip_flag == ISAL_ZLIB_NO_HDR_VER)
				offset = zlib_trl_bytes;

			if (!ret)
				ret =
				    check_zlib_trl(load_u32(state.next_in - offset),
						   state.crc, uncompress_buf, *uncompress_len);
			else if (ret == ISAL_INCORRECT_CHECKSUM)
				ret = INCORRECT_ZLIB_TRAILER;
			state.avail_in -= (zlib_trl_bytes - offset);

		}

	}

	if (ret == 0 && state.avail_in != 0)
		ret = INFLATE_LEFTOVER_INPUT;

	return ret;
}

/* Check if that the state of the data stream is consistent */
int inflate_state_valid_check(struct inflate_state *state, uint8_t * in_buf, uint32_t in_size,
			      uint8_t * out_buf, uint32_t out_size, uint32_t in_processed,
			      uint32_t out_processed, uint32_t data_size)
{
	uint32_t in_buffer_size, total_out, out_buffer_size;

	in_buffer_size = (in_size == 0) ? 0 : state->next_in - in_buf + state->avail_in;

	/* Check for a consistent amount of data processed */
	if (in_buffer_size != in_size)
		return INFLATE_INPUT_STREAM_INTEGRITY_ERROR;

	total_out =
	    (out_size == 0) ? out_processed : out_processed + (state->next_out - out_buf);
	out_buffer_size = (out_size == 0) ? 0 : state->next_out - out_buf + state->avail_out;

	/* Check for a consistent amount of data compressed */
	if (total_out != state->total_out || out_buffer_size != out_size)
		return INFLATE_OUTPUT_STREAM_INTEGRITY_ERROR;

	return 0;
}

/* Performs compression with checks to discover and verify the state of the
 * stream
 * state: inflate data structure which has been initialized to use
 * in_buf and out_buf as the buffers
 * compress_len: size of all input compressed data
 * data_size: size of all available output buffers
 * in_buf: next buffer of data to be inflated
 * in_size: size of in_buf
 * out_buf: next out put buffer where data is stored
 * out_size: size of out_buf
 * in_processed: the amount of input data which has been loaded into buffers
 * to be inflated, this includes the data in in_buf
 * out_processed: the amount of output data which has been decompressed and stored,
 * this does not include the data in the current out_buf
*/
int isal_inflate_with_checks(struct inflate_state *state, uint32_t compress_len,
			     uint32_t data_size, uint8_t * in_buf, uint32_t in_size,
			     uint32_t in_processed, uint8_t * out_buf, uint32_t out_size,
			     uint32_t out_processed)
{
	int ret, stream_check = 0;

	ret = isal_inflate(state);

	/* Verify the stream is in a valid state when no errors occured */
	if (ret >= 0) {
		stream_check =
		    inflate_state_valid_check(state, in_buf, in_size, out_buf, out_size,
					      in_processed, out_processed, data_size);
	}

	if (stream_check != 0)
		return stream_check;

	return ret;

}

int inflate_multi_pass(uint8_t * compress_buf, uint64_t compress_len,
		       uint8_t * uncompress_buf, uint32_t * uncompress_len, uint32_t gzip_flag,
		       uint8_t * dict, uint32_t dict_len, uint32_t hist_bits)
{
	struct inflate_state *state = NULL;
	int ret = 0;
	uint8_t *comp_tmp = NULL, *uncomp_tmp = NULL;
	uint32_t comp_tmp_size = 0, uncomp_tmp_size = 0;
	uint32_t comp_processed = 0, uncomp_processed = 0;
	int32_t read_in_old = 0;
	uint32_t reset_test_flag = 0;

	state = malloc(sizeof(struct inflate_state));
	if (state == NULL) {
		printf("Failed to allocate memory\n");
		exit(0);
	}

	create_rand_repeat_data((uint8_t *) state, sizeof(state));
	isal_inflate_init(state);

	if (rand() % 4 == 0) {
		/* Test reset */
		reset_test_flag = 1;
		create_rand_repeat_data((uint8_t *) state, sizeof(state));
	}

	if (gzip_flag == IGZIP_GZIP_NO_HDR) {
		if (rand() % 2 == 0)
			compress_len -= gzip_trl_bytes;
		else
			gzip_flag = ISAL_GZIP_NO_HDR_VER;
	} else if (gzip_flag == IGZIP_ZLIB_NO_HDR) {
		if (rand() % 2 == 0)
			compress_len -= zlib_trl_bytes;
		else
			gzip_flag = ISAL_ZLIB_NO_HDR_VER;
	}

	state->next_in = NULL;
	state->next_out = NULL;
	state->avail_in = 0;
	state->avail_out = 0;
	state->crc_flag = gzip_flag;
	state->hist_bits = hist_bits;

	if (reset_test_flag)
		isal_inflate_reset(state);

	if (dict != NULL)
		isal_inflate_set_dict(state, dict, dict_len);

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
					return MALLOC_FAILED;
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
					return MALLOC_FAILED;
				}

				state->avail_out = uncomp_tmp_size;
				state->next_out = uncomp_tmp;
			}
		}

		log_print("Pre inflate\n");
		log_print
		    ("compressed_size = 0x%05lx, in_processed = 0x%05x, in_size = 0x%05x, avail_in = 0x%05x\n",
		     compress_len, comp_processed, comp_tmp_size, state->avail_in);
		log_print
		    ("data_size       = 0x%05x, out_processed  = 0x%05x, out_size  = 0x%05x, avail_out  = 0x%05x, total_out  = 0x%05x\n",
		     *uncompress_len, uncomp_processed, uncomp_tmp_size, state->avail_out,
		     state->total_out);

		ret = isal_inflate_with_checks(state, compress_len, *uncompress_len, comp_tmp,
					       comp_tmp_size, comp_processed, uncomp_tmp,
					       uncomp_tmp_size, uncomp_processed);

		log_print("Post inflate\n");
		log_print
		    ("compressed_size = 0x%05lx, in_processed = 0x%05x, in_size = 0x%05x, avail_in = 0x%05x\n",
		     compress_len, comp_processed, comp_tmp_size, state->avail_in);
		log_print
		    ("data_size       = 0x%05x, out_processed  = 0x%05x, out_size  = 0x%05x, avail_out  = 0x%05x, total_out  = 0x%05x\n",
		     *uncompress_len, uncomp_processed, uncomp_tmp_size, state->avail_out,
		     state->total_out);

		if (state->block_state == ISAL_BLOCK_FINISH || ret != 0) {
			memcpy(uncompress_buf + uncomp_processed, uncomp_tmp, uncomp_tmp_size);
			*uncompress_len = state->total_out;
			break;
		}

		if (*uncompress_len - uncomp_processed == 0 && state->avail_out == 0
		    && state->tmp_out_valid - state->tmp_out_processed > 0) {
			ret = ISAL_OUT_OVERFLOW;
			break;
		}

		if (compress_len - comp_processed == 0 && state->avail_in == 0
		    && (state->block_state != ISAL_BLOCK_INPUT_DONE)
		    && state->tmp_out_valid - state->tmp_out_processed == 0) {
			if (state->read_in_length == read_in_old) {
				ret = ISAL_END_INPUT;
				break;
			}
			read_in_old = state->read_in_length;
		}
	}

	if (gzip_flag) {
		if (!ret) {
			if (gzip_flag == IGZIP_GZIP || gzip_flag == IGZIP_GZIP_NO_HDR
			    || gzip_flag == ISAL_GZIP_NO_HDR_VER) {
				if (gzip_flag == ISAL_GZIP_NO_HDR_VER
				    || gzip_flag == IGZIP_GZIP)
					compress_len -= gzip_trl_bytes;
				ret =
				    check_gzip_trl(load_u64(compress_buf + compress_len),
						   state->crc, uncompress_buf,
						   *uncompress_len);
			} else if (gzip_flag == IGZIP_ZLIB_NO_HDR) {
				if (gzip_flag == IGZIP_ZLIB
				    || gzip_flag == ISAL_ZLIB_NO_HDR_VER)
					compress_len -= zlib_trl_bytes;
				ret =
				    check_zlib_trl(load_u32(compress_buf + compress_len),
						   state->crc, uncompress_buf,
						   *uncompress_len);
			}
		}
	}
	if (ret == 0 && state->avail_in != 0)
		ret = INFLATE_LEFTOVER_INPUT;

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

int inflate_ret_to_code(int ret)
{
	switch (ret) {
	case ISAL_DECOMP_OK:
		return 0;
	case ISAL_END_INPUT:
		return INFLATE_END_OF_INPUT;
	case ISAL_OUT_OVERFLOW:
		return INFLATE_OUT_BUFFER_OVERFLOW;
	case ISAL_INVALID_BLOCK:
		return INFLATE_INVALID_BLOCK_HEADER;
	case ISAL_INVALID_SYMBOL:
		return INFLATE_INVALID_SYMBOL;
	case ISAL_INVALID_LOOKBACK:
		return INFLATE_INVALID_LOOK_BACK_DISTANCE;
	default:
		return INFLATE_GENERAL_ERROR;
	}
}

/* Inflate the  compressed data and check that the decompressed data agrees with the input data */
int inflate_check(uint8_t * z_buf, uint32_t z_size, uint8_t * in_buf, uint32_t in_size,
		  uint32_t gzip_flag, uint8_t * dict, uint32_t dict_len, uint32_t hist_bits)
{
	/* Test inflate with reference inflate */

	int ret = 0;
	uint32_t test_size = in_size;
	uint8_t *test_buf = NULL;
	int mem_result = 0;
	int gzip_hdr_result = 0, gzip_trl_result = 0;

	if (in_size > 0) {
		assert(in_buf != NULL);
		test_buf = malloc(test_size);
		if (test_buf == NULL)
			return MALLOC_FAILED;
	}

	if (test_buf != NULL)
		memset(test_buf, 0xff, test_size);

	if (inflate_type == 0 && dict == NULL) {
		ret = inflate_stateless_pass(z_buf, z_size, test_buf, &test_size, gzip_flag);
		inflate_type = 1;
	} else {
		ret =
		    inflate_multi_pass(z_buf, z_size, test_buf, &test_size, gzip_flag, dict,
				       dict_len, hist_bits);
		inflate_type = 0;
	}

	if (test_buf != NULL)
		mem_result = memcmp(in_buf, test_buf, in_size);

	if (options.verbose && mem_result) {
		int i;
		for (i = 0; i < in_size; i++) {
			if (in_buf[i] != test_buf[i]) {
				log_print
				    ("First incorrect data at 0x%x of 0x%x, 0x%x != 0x%x\n", i,
				     in_size, in_buf[i], test_buf[i]);
				break;
			}
		}
	}

	if (test_buf != NULL)
		free(test_buf);
	switch (ret) {
	case 0:
		break;
	case ISAL_END_INPUT:
		return INFLATE_END_OF_INPUT;
		break;
	case ISAL_INVALID_BLOCK:
		return INFLATE_INVALID_BLOCK_HEADER;
		break;
	case ISAL_INVALID_SYMBOL:
		return INFLATE_INVALID_SYMBOL;
		break;
	case ISAL_OUT_OVERFLOW:
		return INFLATE_OUT_BUFFER_OVERFLOW;
		break;
	case ISAL_INVALID_LOOKBACK:
		return INFLATE_INVALID_LOOK_BACK_DISTANCE;
		break;
	case INFLATE_LEFTOVER_INPUT:
		return INFLATE_LEFTOVER_INPUT;
		break;
	case INCORRECT_GZIP_TRAILER:
		gzip_trl_result = INCORRECT_GZIP_TRAILER;
		break;
	case INCORRECT_ZLIB_TRAILER:
		gzip_trl_result = INCORRECT_ZLIB_TRAILER;
		break;
	case ISAL_INCORRECT_CHECKSUM:
		if (gzip_flag == IGZIP_GZIP || gzip_flag == IGZIP_GZIP_NO_HDR
		    || gzip_flag == ISAL_GZIP_NO_HDR_VER)
			gzip_trl_result = INCORRECT_GZIP_TRAILER;
		else if (gzip_flag == IGZIP_ZLIB || gzip_flag == IGZIP_ZLIB_NO_HDR
			 || gzip_flag == ISAL_ZLIB_NO_HDR_VER)
			gzip_trl_result = INCORRECT_GZIP_TRAILER;
		break;
	case ISAL_UNSUPPORTED_METHOD:
		return UNSUPPORTED_METHOD;
	case INFLATE_INPUT_STREAM_INTEGRITY_ERROR:
		return INFLATE_INPUT_STREAM_INTEGRITY_ERROR;
		break;
	case INFLATE_OUTPUT_STREAM_INTEGRITY_ERROR:
		return INFLATE_OUTPUT_STREAM_INTEGRITY_ERROR;
		break;
	default:
		return INFLATE_GENERAL_ERROR;
		break;
	}

	if (test_size != in_size)
		return INFLATE_INCORRECT_OUTPUT_SIZE;

	if (mem_result)
		return RESULT_ERROR;

	if (gzip_hdr_result == INVALID_GZIP_HEADER)
		return INVALID_GZIP_HEADER;

	else if (gzip_hdr_result == INVALID_ZLIB_HEADER)
		return INVALID_ZLIB_HEADER;

	if (gzip_trl_result == INCORRECT_GZIP_TRAILER)
		return INCORRECT_GZIP_TRAILER;

	else if (gzip_trl_result == INCORRECT_ZLIB_TRAILER)
		return INCORRECT_ZLIB_TRAILER;

	return 0;
}

/* Check if that the state of the data stream is consistent */
int stream_valid_check(struct isal_zstream *stream, uint8_t * in_buf, uint32_t in_size,
		       uint8_t * out_buf, uint32_t out_size, uint32_t in_processed,
		       uint32_t out_processed, uint32_t data_size)
{
	uint32_t total_in, in_buffer_size, total_out, out_buffer_size;

	total_in =
	    (in_size ==
	     0) ? in_processed : (in_processed - in_size) + (stream->next_in - in_buf);
	in_buffer_size = (in_size == 0) ? 0 : stream->next_in - in_buf + stream->avail_in;

	/* Check for a consistent amount of data processed */
	if (total_in != stream->total_in || in_buffer_size != in_size)
		return COMPRESS_INPUT_STREAM_INTEGRITY_ERROR;

	total_out =
	    (out_size == 0) ? out_processed : out_processed + (stream->next_out - out_buf);
	out_buffer_size = (out_size == 0) ? 0 : stream->next_out - out_buf + stream->avail_out;

	/* Check for a consistent amount of data compressed */
	if (total_out != stream->total_out || out_buffer_size != out_size) {
		return COMPRESS_OUTPUT_STREAM_INTEGRITY_ERROR;
	}

	return 0;
}

/* Performs compression with checks to discover and verify the state of the
 * stream
 * stream: compress data structure which has been initialized to use
 * in_buf and out_buf as the buffers
 * data_size: size of all input data
 * compressed_size: size of all available output buffers
 * in_buf: next buffer of data to be compressed
 * in_size: size of in_buf
 * out_buf: next out put buffer where data is stored
 * out_size: size of out_buf
 * in_processed: the amount of input data which has been loaded into buffers
 * to be compressed, this includes the data in in_buf
 * out_processed: the amount of output data which has been compressed and stored,
 * this does not include the data in the current out_buf
*/
int isal_deflate_with_checks(struct isal_zstream *stream, uint32_t data_size,
			     uint32_t compressed_size, uint8_t * in_buf, uint32_t in_size,
			     uint32_t in_processed, uint8_t * out_buf, uint32_t out_size,
			     uint32_t out_processed)
{
	int ret, stream_check;
	struct isal_zstate *state = &stream->internal_state;

	log_print("Pre compression\n");
	log_print
	    ("data_size       = 0x%05x, in_processed  = 0x%05x, in_size  = 0x%05x, avail_in  = 0x%05x, total_in  = 0x%05x\n",
	     data_size, in_processed, in_size, stream->avail_in, stream->total_in);
	log_print
	    ("compressed_size = 0x%05x, out_processed = 0x%05x, out_size = 0x%05x, avail_out = 0x%05x, total_out = 0x%05x\n",
	     compressed_size, out_processed, out_size, stream->avail_out, stream->total_out);

	ret = isal_deflate(stream);

	log_print("Post compression\n");
	log_print
	    ("data_size       = 0x%05x, in_processed  = 0x%05x, in_size  = 0x%05x, avail_in  = 0x%05x, total_in  = 0x%05x\n",
	     data_size, in_processed, in_size, stream->avail_in, stream->total_in);
	log_print
	    ("compressed_size = 0x%05x, out_processed = 0x%05x, out_size = 0x%05x, avail_out = 0x%05x, total_out = 0x%05x\n",
	     compressed_size, out_processed, out_size, stream->avail_out, stream->total_out);
	log_print("\n\n");

	/* Verify the stream is in a valid state */
	stream_check = stream_valid_check(stream, in_buf, in_size, out_buf, out_size,
					  in_processed, out_processed, data_size);

	if (stream_check != 0)
		return stream_check;

	if (ret != IGZIP_COMP_OK)
		return COMPRESS_GENERAL_ERROR;

	/* Check if the compression is completed */
	if (state->state != ZSTATE_END)
		if (compressed_size - out_processed - (out_size - stream->avail_out) <= 0)
			return COMPRESS_OUT_BUFFER_OVERFLOW;

	return ret;

}

void set_random_hufftable(struct isal_zstream *stream, int level, uint8_t * data,
			  uint32_t data_size)
{
	struct isal_hufftables *huff = hufftables;
	struct isal_huff_histogram hist;
	if (level == 0 || rand() % 16 == 0) {
		if (rand() % 8 == 0) {
			huff = hufftables_subset;
			memset(&hist, 0, sizeof(hist));
			isal_update_histogram(data, data_size, &hist);
			isal_create_hufftables_subset(huff, &hist);
		}

		isal_deflate_set_hufftables(stream, huff, rand() % 4);
	}
}

/* Compress the input data into the output buffer where the input buffer and
 * output buffer are randomly segmented to test state information for the
 * compression*/
int compress_multi_pass(uint8_t * data, uint32_t data_size, uint8_t * compressed_buf,
			uint32_t * compressed_size, uint32_t flush_type, uint32_t gzip_flag,
			uint32_t level, uint8_t * dict, uint32_t dict_len, uint32_t hist_bits)
{
	int ret = IGZIP_COMP_OK;
	uint8_t *in_buf = NULL, *out_buf = NULL;
	uint32_t in_size = 0, out_size = 0;
	uint32_t in_processed = 0, out_processed = 0;
	struct isal_zstream *stream;
	struct isal_zstate *state;
	uint32_t loop_count = 0;
	uint32_t level_buf_size;
	uint8_t *level_buf = NULL;
	struct isal_hufftables *huff_tmp;
	uint32_t reset_test_flag = 0;
	uint8_t tmp_symbol;
	int no_mod = 0;
	struct isal_dict dict_str;

	log_print("Starting Compress Multi Pass\n");

	stream = malloc(sizeof(*stream));
	if (stream == NULL)
		return MALLOC_FAILED;
	state = &stream->internal_state;

	create_rand_repeat_data((uint8_t *) stream, sizeof(*stream));

	isal_deflate_init(stream);

	if (state->state != ZSTATE_NEW_HDR)
		return COMPRESS_INCORRECT_STATE;

	if (rand() % 4 == 0) {
		/* Test reset */
		reset_test_flag = 1;
		huff_tmp = stream->hufftables;
		create_rand_repeat_data((uint8_t *) stream, sizeof(*stream));

		/* Restore variables not necessarily set by user */
		stream->hufftables = huff_tmp;
		stream->end_of_stream = 0;
		stream->level = 0;
		stream->level_buf = NULL;
		stream->level_buf_size = 0;
	}

	stream->flush = flush_type;
	stream->end_of_stream = 0;

	/* These are set here to allow the loop to run correctly */
	stream->avail_in = 0;
	stream->avail_out = 0;
	stream->gzip_flag = gzip_flag;
	stream->level = level;
	stream->hist_bits = hist_bits;

	if (level >= 1) {
		level_buf_size = get_rand_level_buf_size(stream->level);
		level_buf = malloc(level_buf_size);
		create_rand_repeat_data(level_buf, level_buf_size);
		stream->level_buf = level_buf;
		stream->level_buf_size = level_buf_size;
	}

	if (reset_test_flag)
		isal_deflate_reset(stream);

	if (dict != NULL) {
		if (rand() % 2 == 0)
			isal_deflate_set_dict(stream, dict, dict_len);
		else {
			isal_deflate_process_dict(stream, &dict_str, dict, dict_len);
			isal_deflate_reset_dict(stream, &dict_str);
		}
	}

	while (1) {
		loop_count++;

		/* Setup in buffer for next round of compression */
		if (stream->avail_in == 0) {
			if (flush_type == NO_FLUSH || state->state == ZSTATE_NEW_HDR) {
				/* Randomly choose size of the next out buffer */
				in_size = rand() % (data_size + 1);

				/* Limit size of buffer to be smaller than maximum */
				if (in_size >= data_size - in_processed) {
					in_size = data_size - in_processed;
					stream->end_of_stream = 1;
				}

				if (in_size != 0) {
					if (in_buf != NULL) {
						free(in_buf);
						in_buf = NULL;
					}

					in_buf = malloc(in_size);
					if (in_buf == NULL) {
						ret = MALLOC_FAILED;
						break;
					}
					memcpy(in_buf, data + in_processed, in_size);
					in_processed += in_size;

					stream->avail_in = in_size;
					stream->next_in = in_buf;
				}
			}
		} else {
			/* Randomly modify data after next in */
			if (rand() % 4 == 0 && !no_mod) {

				tmp_symbol = rand();
				log_print
				    ("Modifying data at index 0x%x from 0x%x to 0x%x before recalling isal_deflate\n",
				     in_processed - stream->avail_in,
				     data[in_processed - stream->avail_in], tmp_symbol);
				*stream->next_in = tmp_symbol;
				data[in_processed - stream->avail_in] = tmp_symbol;
			}
		}

		/* Setup out buffer for next round of compression */
		if (stream->avail_out == 0) {
			/* Save compressed data inot compressed_buf */
			if (out_buf != NULL) {
				memcpy(compressed_buf + out_processed, out_buf,
				       out_size - stream->avail_out);
				out_processed += out_size - stream->avail_out;
			}

			/* Randomly choose size of the next out buffer */
			out_size = rand() % (*compressed_size + 1);

			/* Limit size of buffer to be smaller than maximum */
			if (out_size > *compressed_size - out_processed)
				out_size = *compressed_size - out_processed;

			if (out_size != 0) {
				if (out_buf != NULL) {
					free(out_buf);
					out_buf = NULL;
				}

				out_buf = malloc(out_size);
				if (out_buf == NULL) {
					ret = MALLOC_FAILED;
					break;
				}

				stream->avail_out = out_size;
				stream->next_out = out_buf;
			}
		}

		if (state->state == ZSTATE_NEW_HDR) {
			set_random_hufftable(stream, level, data, data_size);
			if (stream->hufftables == hufftables_subset)
				no_mod = 1;
			else
				no_mod = 0;
		}

		ret =
		    isal_deflate_with_checks(stream, data_size, *compressed_size, in_buf,
					     in_size, in_processed, out_buf, out_size,
					     out_processed);

		if (ret) {
			if (ret == COMPRESS_OUT_BUFFER_OVERFLOW
			    || ret == COMPRESS_INCORRECT_STATE)
				memcpy(compressed_buf + out_processed, out_buf, out_size);
			break;
		}

		/* Check if the compression is completed */
		if (state->state == ZSTATE_END) {
			memcpy(compressed_buf + out_processed, out_buf, out_size);
			*compressed_size = stream->total_out;
			break;
		}

	}

	if (stream != NULL)
		free(stream);
	if (level_buf != NULL)
		free(level_buf);
	if (in_buf != NULL)
		free(in_buf);
	if (out_buf != NULL)
		free(out_buf);

	if (ret == COMPRESS_OUT_BUFFER_OVERFLOW && flush_type == SYNC_FLUSH
	    && loop_count >= MAX_LOOPS)
		ret = COMPRESS_LOOP_COUNT_OVERFLOW;

	return ret;

}

/* Compress the input data into the outbuffer in one call to isal_deflate */
int compress_single_pass(uint8_t * data, uint32_t data_size, uint8_t * compressed_buf,
			 uint32_t * compressed_size, uint32_t flush_type, uint32_t gzip_flag,
			 uint32_t level, uint8_t * dict, uint32_t dict_len, uint32_t hist_bits)
{
	int ret = IGZIP_COMP_OK;
	struct isal_zstream stream;
	struct isal_zstate *state = &stream.internal_state;
	uint32_t level_buf_size;
	uint8_t *level_buf = NULL;
	struct isal_hufftables *huff_tmp;
	uint32_t reset_test_flag = 0;
	struct isal_dict dict_str;

	log_print("Starting Compress Single Pass\n");

	create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

	isal_deflate_init(&stream);

	set_random_hufftable(&stream, level, data, data_size);

	if (state->state != ZSTATE_NEW_HDR)
		return COMPRESS_INCORRECT_STATE;

	if (rand() % 4 == 0) {
		/* Test reset */
		reset_test_flag = 1;
		huff_tmp = stream.hufftables;
		create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

		/* Restore variables not necessarily set by user */
		stream.hufftables = huff_tmp;
		stream.end_of_stream = 0;
		stream.level = 0;
		stream.level_buf = NULL;
		stream.level_buf_size = 0;
	}

	stream.flush = flush_type;
	stream.avail_in = data_size;
	stream.next_in = data;
	stream.avail_out = *compressed_size;
	stream.next_out = compressed_buf;
	stream.end_of_stream = 1;
	stream.gzip_flag = gzip_flag;
	stream.level = level;
	stream.hist_bits = hist_bits;

	if (level >= 1) {
		level_buf_size = get_rand_level_buf_size(stream.level);
		level_buf = malloc(level_buf_size);
		create_rand_repeat_data(level_buf, level_buf_size);
		stream.level_buf = level_buf;
		stream.level_buf_size = level_buf_size;
	}

	if (reset_test_flag)
		isal_deflate_reset(&stream);

	if (dict != NULL) {
		if (rand() % 2 == 0)
			isal_deflate_set_dict(&stream, dict, dict_len);
		else {
			isal_deflate_process_dict(&stream, &dict_str, dict, dict_len);
			isal_deflate_reset_dict(&stream, &dict_str);
		}
	}

	ret =
	    isal_deflate_with_checks(&stream, data_size, *compressed_size, data, data_size,
				     data_size, compressed_buf, *compressed_size, 0);

	if (level_buf != NULL)
		free(level_buf);

	/* Check if the compression is completed */
	if (state->state == ZSTATE_END)
		*compressed_size = stream.total_out;
	else if (flush_type == SYNC_FLUSH && stream.avail_out < 16)
		ret = COMPRESS_OUT_BUFFER_OVERFLOW;

	return ret;

}

/* Compress the input data repeatedly into the outbuffer
 * Compresses and verifies in place to decrease memory usage
 */
int compress_ver_rep_buf(uint8_t * data, uint32_t data_size, uint64_t data_rep_size,
			 uint8_t * compressed_buf, uint32_t compressed_size,
			 uint8_t * decomp_buf, uint32_t decomp_buf_size, uint32_t flush_type,
			 uint32_t gzip_flag, uint32_t level)
{
	int ret = IGZIP_COMP_OK;
	struct isal_zstream stream;
	struct inflate_state state;
	uint32_t level_buf_size;
	uint8_t *level_buf = NULL;
	uint64_t data_remaining = data_rep_size;
	uint64_t data_verified = 0;
	uint32_t index;
	uint32_t out_size, cmp_size;
	uint32_t avail_out_start;

	log_print("Starting Compress and Verify Repeated Buffer\n");

	create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

	/* Setup compression stream */
	isal_deflate_init(&stream);
	stream.avail_in = 0;
	stream.next_in = NULL;
	stream.avail_out = 0;
	stream.next_out = NULL;

	set_random_hufftable(&stream, level, data, data_size);
	stream.flush = flush_type;
	stream.end_of_stream = 0;
	stream.gzip_flag = gzip_flag;
	stream.level = level;

	if (level >= 1) {
		level_buf_size = get_rand_level_buf_size(stream.level);
		level_buf = malloc(level_buf_size);
		create_rand_repeat_data(level_buf, level_buf_size);
		stream.level_buf = level_buf;
		stream.level_buf_size = level_buf_size;
	}

	/* Setup decompression stream */
	create_rand_repeat_data((uint8_t *) & state, sizeof(state));
	isal_inflate_init(&state);
	state.crc_flag = gzip_flag;

	while (data_remaining || stream.avail_in) {
		/* Compress the input buffer */
		if (stream.next_out == NULL) {
			stream.avail_out = compressed_size;
			stream.next_out = compressed_buf;
		}

		while (stream.avail_out > 0 && (data_remaining || stream.avail_in)) {
			if (stream.avail_in == 0) {
				stream.avail_in = data_size;
				if (data_size >= data_remaining) {
					stream.avail_in = data_remaining;
					stream.end_of_stream = 1;
				}

				stream.next_in = data;
				data_remaining -= stream.avail_in;
			}

			ret = isal_deflate(&stream);

			if (ret)
				return COMPRESS_GENERAL_ERROR;
		}

		/* Verfiy the compressed buffer */
		state.next_in = compressed_buf;
		state.avail_in = compressed_size;
		state.next_out = NULL;
		state.avail_out = 0;
		create_rand_repeat_data(decomp_buf, decomp_buf_size);

		while (state.avail_out == 0) {
			state.next_out = decomp_buf;
			state.avail_out = decomp_buf_size;

			/* Force decoding to stop when avail_out rolls over */
			if ((1ULL << 32) - state.total_out < decomp_buf_size)
				state.avail_out = (1ULL << 32) - state.total_out;

			avail_out_start = state.avail_out;

			ret = isal_inflate(&state);
			if (ret)
				return inflate_ret_to_code(ret);

			/* Check data accuracy */
			index = data_verified % data_size;
			out_size = avail_out_start - state.avail_out;
			cmp_size =
			    (out_size > data_size - index) ? data_size - index : out_size;
			ret |= memcmp(decomp_buf, data + index, cmp_size);
			out_size -= cmp_size;
			cmp_size = (out_size > index) ? index : out_size;
			ret |= memcmp(decomp_buf + data_size - index, data, cmp_size);
			out_size -= cmp_size;
			cmp_size = out_size;
			ret |= memcmp(decomp_buf, decomp_buf + data_size, out_size);
			if (ret)
				return RESULT_ERROR;

			data_verified += avail_out_start - state.avail_out;
		}
		stream.next_out = NULL;
	}

	if (level_buf != NULL)
		free(level_buf);

	return ret;

}

/* Statelessly compress the input buffer into the output buffer */
int compress_stateless(uint8_t * data, uint32_t data_size, uint8_t * compressed_buf,
		       uint32_t * compressed_size, uint32_t flush_type, uint32_t gzip_flag,
		       uint32_t level, uint32_t hist_bits)
{
	int ret = IGZIP_COMP_OK;
	struct isal_zstream stream;
	uint32_t level_buf_size;
	uint8_t *level_buf = NULL;
	struct isal_hufftables *huff_tmp;
	uint32_t reset_test_flag = 0;

	create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

	isal_deflate_stateless_init(&stream);

	set_random_hufftable(&stream, level, data, data_size);

	if (rand() % 4 == 0) {
		/* Test reset */
		reset_test_flag = 1;
		huff_tmp = stream.hufftables;
		create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

		/* Restore variables not necessarily set by user */
		stream.hufftables = huff_tmp;
		stream.end_of_stream = 0;
		stream.level = 0;
		stream.level_buf = NULL;
		stream.level_buf_size = 0;
	}

	stream.avail_in = data_size;
	stream.next_in = data;
	stream.flush = flush_type;
	if (flush_type != NO_FLUSH)
		stream.end_of_stream = 1;
	stream.avail_out = *compressed_size;
	stream.next_out = compressed_buf;
	stream.gzip_flag = gzip_flag;
	stream.level = level;
	stream.hist_bits = hist_bits;

	if (level == 1) {
		/* This is to test case where level buf uses already existing
		 * internal buffers */
		level_buf_size = rand() % IBUF_SIZE;

		if (level_buf_size >= ISAL_DEF_LVL1_MIN) {
			level_buf = malloc(level_buf_size);
			create_rand_repeat_data(level_buf, level_buf_size);
			stream.level_buf = level_buf;
			stream.level_buf_size = level_buf_size;
		}
	} else if (level > 1) {
		level_buf_size = get_rand_level_buf_size(level);
		level_buf = malloc(level_buf_size);
		create_rand_repeat_data(level_buf, level_buf_size);
		stream.level_buf = level_buf;
		stream.level_buf_size = level_buf_size;
	}

	if (reset_test_flag)
		isal_deflate_reset(&stream);

	ret = isal_deflate_stateless(&stream);

	if (level_buf != NULL)
		free(level_buf);

	/* verify the stream */
	if (stream.next_in - data != stream.total_in ||
	    stream.total_in + stream.avail_in != data_size)
		return COMPRESS_INPUT_STREAM_INTEGRITY_ERROR;

	if (stream.next_out - compressed_buf != stream.total_out ||
	    stream.total_out + stream.avail_out != *compressed_size) {
		return COMPRESS_OUTPUT_STREAM_INTEGRITY_ERROR;
	}

	if (ret != IGZIP_COMP_OK) {
		if (ret == STATELESS_OVERFLOW)
			return COMPRESS_OUT_BUFFER_OVERFLOW;
		else if (ret == INVALID_FLUSH)
			return INVALID_FLUSH_ERROR;
		else {
			printf("Return due to ret = %d with level = %d or %d\n", ret, level,
			       stream.level);
			return COMPRESS_GENERAL_ERROR;
		}
	}

	if (!stream.end_of_stream) {
		return COMPRESS_END_OF_STREAM_NOT_SET;
	}

	if (stream.avail_in != 0)
		return COMPRESS_ALL_INPUT_FAIL;

	*compressed_size = stream.total_out;

	return ret;

}

/* Statelessly compress the input buffer into the output buffer */
int compress_stateless_full_flush(uint8_t * data, uint32_t data_size, uint8_t * compressed_buf,
				  uint32_t * compressed_size, uint32_t level,
				  uint32_t hist_bits)
{
	int ret = IGZIP_COMP_OK;
	uint8_t *in_buf = NULL, *level_buf = NULL, *out_buf = compressed_buf;
	uint32_t in_size = 0, level_buf_size;
	uint32_t in_processed = 00;
	struct isal_zstream stream;
	uint32_t loop_count = 0;
	struct isal_hufftables *huff_tmp;
	uint32_t reset_test_flag = 0;

	log_print("Starting Stateless Compress Full Flush\n");

	create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

	isal_deflate_stateless_init(&stream);

	if (rand() % 4 == 0) {
		/* Test reset */
		reset_test_flag = 1;
		huff_tmp = stream.hufftables;
		create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

		/* Restore variables not necessarily set by user */
		stream.hufftables = huff_tmp;
		stream.end_of_stream = 0;
		stream.level = 0;
		stream.level_buf = NULL;
		stream.level_buf_size = 0;
		stream.gzip_flag = 0;
	}

	stream.flush = FULL_FLUSH;
	stream.end_of_stream = 0;
	stream.avail_out = *compressed_size;
	stream.next_out = compressed_buf;
	stream.level = level;
	stream.hist_bits = hist_bits;

	if (level == 1) {
		/* This is to test case where level_buf uses already existing
		 * internal buffers */
		level_buf_size = rand() % IBUF_SIZE;

		if (level_buf_size >= ISAL_DEF_LVL1_MIN) {
			level_buf = malloc(level_buf_size);
			create_rand_repeat_data(level_buf, level_buf_size);
			stream.level_buf = level_buf;
			stream.level_buf_size = level_buf_size;
		}
	} else if (level > 1) {
		level_buf_size = get_rand_level_buf_size(level);
		level_buf = malloc(level_buf_size);
		create_rand_repeat_data(level_buf, level_buf_size);
		stream.level_buf = level_buf;
		stream.level_buf_size = level_buf_size;
	}

	if (reset_test_flag)
		isal_deflate_reset(&stream);

	while (1) {
		loop_count++;

		/* Randomly choose size of the next out buffer */
		in_size = rand() % (data_size + 1);

		/* Limit size of buffer to be smaller than maximum */
		if (in_size >= data_size - in_processed) {
			in_size = data_size - in_processed;
			stream.end_of_stream = 1;
		}

		stream.avail_in = in_size;

		if (in_size != 0) {
			if (in_buf != NULL) {
				free(in_buf);
				in_buf = NULL;
			}

			in_buf = malloc(in_size);
			if (in_buf == NULL) {
				ret = MALLOC_FAILED;
				break;
			}
			memcpy(in_buf, data + in_processed, in_size);
			in_processed += in_size;

			stream.next_in = in_buf;
		}

		out_buf = stream.next_out;

		if (stream.internal_state.state == ZSTATE_NEW_HDR)
			set_random_hufftable(&stream, level, data, data_size);

		ret = isal_deflate_stateless(&stream);

		assert(stream.internal_state.bitbuf.m_bit_count == 0);

		assert(compressed_buf == stream.next_out - stream.total_out);
		if (ret)
			break;

		/* Verify that blocks are independent */
		ret =
		    inflate_check(out_buf, stream.next_out - out_buf, in_buf, in_size, 0, NULL,
				  0, hist_bits);

		if (ret == INFLATE_INVALID_LOOK_BACK_DISTANCE) {
			break;
		} else
			ret = 0;

		/* Check if the compression is completed */
		if (in_processed == data_size) {
			*compressed_size = stream.total_out;
			break;
		}

	}

	if (level_buf != NULL)
		free(level_buf);

	if (in_buf != NULL)
		free(in_buf);

	if (ret == STATELESS_OVERFLOW && loop_count >= MAX_LOOPS)
		ret = COMPRESS_LOOP_COUNT_OVERFLOW;

	return ret;

}

/* Compress the input data into the output buffer where the input buffer and
 * is randomly segmented to test for independence of blocks in full flush
 * compression*/
int compress_full_flush(uint8_t * data, uint32_t data_size, uint8_t * compressed_buf,
			uint32_t * compressed_size, uint32_t gzip_flag, uint32_t level)
{
	int ret = IGZIP_COMP_OK;
	uint8_t *in_buf = NULL, *out_buf = compressed_buf, *level_buf = NULL;
	uint32_t in_size = 0, level_buf_size;
	uint32_t in_processed = 00;
	struct isal_zstream stream;
	struct isal_zstate *state = &stream.internal_state;
	uint32_t loop_count = 0;
	struct isal_hufftables *huff_tmp;
	uint32_t reset_test_flag = 0;

	log_print("Starting Compress Full Flush\n");

	create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

	isal_deflate_init(&stream);

	if (state->state != ZSTATE_NEW_HDR)
		return COMPRESS_INCORRECT_STATE;

	if (rand() % 4 == 0) {
		/* Test reset */
		reset_test_flag = 1;
		huff_tmp = stream.hufftables;
		create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

		/* Restore variables not necessarily set by user */
		stream.hufftables = huff_tmp;
		stream.end_of_stream = 0;
		stream.level = 0;
		stream.level_buf = NULL;
		stream.level_buf_size = 0;
		stream.hist_bits = 0;
	}

	stream.flush = FULL_FLUSH;
	stream.end_of_stream = 0;
	stream.avail_out = *compressed_size;
	stream.next_out = compressed_buf;
	stream.total_out = 0;
	stream.gzip_flag = gzip_flag;
	stream.level = level;

	if (level >= 1) {
		level_buf_size = get_rand_level_buf_size(stream.level);
		if (level_buf_size >= ISAL_DEF_LVL1_MIN) {
			level_buf = malloc(level_buf_size);
			create_rand_repeat_data(level_buf, level_buf_size);
			stream.level_buf = level_buf;
			stream.level_buf_size = level_buf_size;
		}
	}

	if (reset_test_flag)
		isal_deflate_reset(&stream);

	while (1) {
		loop_count++;

		/* Setup in buffer for next round of compression */
		if (state->state == ZSTATE_NEW_HDR) {
			/* Randomly choose size of the next out buffer */
			in_size = rand() % (data_size + 1);

			/* Limit size of buffer to be smaller than maximum */
			if (in_size >= data_size - in_processed) {
				in_size = data_size - in_processed;
				stream.end_of_stream = 1;
			}

			stream.avail_in = in_size;

			if (in_size != 0) {
				if (in_buf != NULL) {
					free(in_buf);
					in_buf = NULL;
				}

				in_buf = malloc(in_size);
				if (in_buf == NULL) {
					ret = MALLOC_FAILED;
					break;
				}
				memcpy(in_buf, data + in_processed, in_size);
				in_processed += in_size;

				stream.next_in = in_buf;
			}

			out_buf = stream.next_out;
		}

		if (state->state == ZSTATE_NEW_HDR)
			set_random_hufftable(&stream, level, data, data_size);

		ret = isal_deflate(&stream);

		if (ret)
			break;

		/* Verify that blocks are independent */
		if (state->state == ZSTATE_NEW_HDR || state->state == ZSTATE_END) {
			ret =
			    inflate_check(out_buf, stream.next_out - out_buf, in_buf, in_size,
					  0, NULL, 0, 0);

			if (ret == INFLATE_INVALID_LOOK_BACK_DISTANCE)
				break;
			else
				ret = 0;
		}

		/* Check if the compression is completed */
		if (state->state == ZSTATE_END) {
			*compressed_size = stream.total_out;
			break;
		}

	}

	if (level_buf != NULL)
		free(level_buf);

	if (in_buf != NULL)
		free(in_buf);

	if (ret == COMPRESS_OUT_BUFFER_OVERFLOW && loop_count >= MAX_LOOPS)
		ret = COMPRESS_LOOP_COUNT_OVERFLOW;

	return ret;

}

/*Compress the input buffer into the output buffer, but switch the flush type in
 * the middle of the compression to test what happens*/
int compress_swap_flush(uint8_t * data, uint32_t data_size, uint8_t * compressed_buf,
			uint32_t * compressed_size, uint32_t flush_type, int level,
			uint32_t gzip_flag)
{
	int ret = IGZIP_COMP_OK;
	struct isal_zstream stream;
	struct isal_zstate *state = &stream.internal_state;
	uint32_t partial_size;
	struct isal_hufftables *huff_tmp;
	uint32_t reset_test_flag = 0;
	uint32_t level_buf_size;
	uint8_t *level_buf = NULL;

	log_print("Starting Compress Swap Flush\n");

	isal_deflate_init(&stream);

	set_random_hufftable(&stream, 0, data, data_size);

	if (state->state != ZSTATE_NEW_HDR)
		return COMPRESS_INCORRECT_STATE;

	if (rand() % 4 == 0) {
		/* Test reset */
		reset_test_flag = 1;
		huff_tmp = stream.hufftables;
		create_rand_repeat_data((uint8_t *) & stream, sizeof(stream));

		/* Restore variables not necessarily set by user */
		stream.hufftables = huff_tmp;
		stream.end_of_stream = 0;
		stream.level = 0;
		stream.level_buf = NULL;
		stream.level_buf_size = 0;
	}

	partial_size = rand() % (data_size + 1);

	stream.flush = flush_type;
	stream.avail_in = partial_size;
	stream.next_in = data;
	stream.avail_out = *compressed_size;
	stream.next_out = compressed_buf;
	stream.end_of_stream = 0;
	stream.gzip_flag = gzip_flag;
	if (level) {
		stream.level = level;
		level_buf_size = get_rand_level_buf_size(stream.level);
		level_buf = malloc(level_buf_size);
		create_rand_repeat_data(level_buf, level_buf_size);
		stream.level_buf = level_buf;
		stream.level_buf_size = level_buf_size;
	}

	if (reset_test_flag)
		isal_deflate_reset(&stream);

	ret =
	    isal_deflate_with_checks(&stream, data_size, *compressed_size, data, partial_size,
				     partial_size, compressed_buf, *compressed_size, 0);

	if (ret)
		return ret;

	if (state->state == ZSTATE_NEW_HDR)
		set_random_hufftable(&stream, 0, data, data_size);

	flush_type = rand() % 3;

	stream.flush = flush_type;
	stream.avail_in = data_size - partial_size;
	stream.next_in = data + partial_size;
	stream.end_of_stream = 1;

	ret =
	    isal_deflate_with_checks(&stream, data_size, *compressed_size, data + partial_size,
				     data_size - partial_size, data_size, compressed_buf,
				     *compressed_size, 0);

	if (ret == COMPRESS_GENERAL_ERROR)
		return INVALID_FLUSH_ERROR;

	*compressed_size = stream.total_out;

	if (stream.level_buf != NULL)
		free(stream.level_buf);

	return ret;
}

/* Test deflate_stateless */
int test_compress_stateless(uint8_t * in_data, uint32_t in_size, uint32_t flush_type)
{
	int ret = IGZIP_COMP_OK;
	uint32_t z_size, overflow, gzip_flag, level, hist_bits;
	uint8_t *z_buf = NULL;
	uint8_t *in_buf = NULL;

	gzip_flag = rand() % 5;
	hist_bits = rand() % 16;
	level = get_rand_level();

	if (in_size != 0) {
		in_buf = malloc(in_size);

		if (in_buf == NULL)
			return MALLOC_FAILED;

		memcpy(in_buf, in_data, in_size);
	}

	/* Test non-overflow case where a type 0 block is not written */
	z_size = 2 * in_size + hdr_bytes;
	if (gzip_flag == IGZIP_GZIP)
		z_size += gzip_extra_bytes;
	else if (gzip_flag == IGZIP_GZIP_NO_HDR)
		z_size += gzip_trl_bytes;
	else if (gzip_flag == IGZIP_ZLIB)
		z_size += zlib_extra_bytes;
	else if (gzip_flag == IGZIP_ZLIB_NO_HDR)
		z_size += zlib_trl_bytes;

	z_buf = malloc(z_size);

	if (z_buf == NULL)
		return MALLOC_FAILED;

	create_rand_repeat_data(z_buf, z_size);

	/* If flush type is invalid */
	if (flush_type != NO_FLUSH && flush_type != FULL_FLUSH) {
		ret =
		    compress_stateless(in_buf, in_size, z_buf, &z_size, flush_type, gzip_flag,
				       level, hist_bits);

		if (ret != INVALID_FLUSH_ERROR)
			print_error(ret);
		else
			ret = 0;

		if (z_buf != NULL)
			free(z_buf);

		if (in_buf != NULL)
			free(in_buf);

		return ret;
	}

	/* Else test valid flush type */
	ret = compress_stateless(in_buf, in_size, z_buf, &z_size, flush_type, gzip_flag, level,
				 hist_bits);

	if (!ret)
		ret =
		    inflate_check(z_buf, z_size, in_buf, in_size, gzip_flag, NULL, 0,
				  hist_bits);

	if (options.verbose && ret) {
		log_print
		    ("Compressed array at level %d with gzip flag %d, flush type %d, and window bits %d: ",
		     level, gzip_flag, flush_type, hist_bits);
		log_uint8_t(z_buf, z_size);
		log_print("\n");
		log_print("Data: ");
		log_uint8_t(in_buf, in_size);
	}

	if (z_buf != NULL) {
		free(z_buf);
		z_buf = NULL;
	}

	print_error(ret);
	if (ret)
		return ret;

	/*Test non-overflow case where a type 0 block is possible to be written */
	z_size = TYPE0_HDR_SIZE * ((in_size + TYPE0_MAX_SIZE - 1) / TYPE0_MAX_SIZE) + in_size;

	if (gzip_flag == IGZIP_GZIP)
		z_size += gzip_extra_bytes;
	else if (gzip_flag == IGZIP_GZIP_NO_HDR)
		z_size += gzip_trl_bytes;
	else if (gzip_flag == IGZIP_ZLIB)
		z_size += zlib_extra_bytes;
	else if (gzip_flag == IGZIP_ZLIB_NO_HDR)
		z_size += zlib_trl_bytes;

	if (z_size <= gzip_extra_bytes)
		z_size += TYPE0_HDR_SIZE;

	if (z_size < 8)
		z_size = 8;

	z_buf = malloc(z_size);

	if (z_buf == NULL)
		return MALLOC_FAILED;

	create_rand_repeat_data(z_buf, z_size);

	ret = compress_stateless(in_buf, in_size, z_buf, &z_size, flush_type, gzip_flag, level,
				 hist_bits);
	if (!ret)
		ret =
		    inflate_check(z_buf, z_size, in_buf, in_size, gzip_flag, NULL, 0,
				  hist_bits);
	if (ret) {
		log_print
		    ("Compressed array at level %d with gzip flag %d, flush type %d, and hist_bits %d: ",
		     level, gzip_flag, flush_type, hist_bits);
		log_uint8_t(z_buf, z_size);
		log_print("\n");
		log_print("Data: ");
		log_uint8_t(in_buf, in_size);
	}

	if (!ret) {
		free(z_buf);
		z_buf = NULL;

		/* Test random overflow case */
		z_size = rand() % z_size;

		if (z_size > in_size)
			z_size = rand() & in_size;

		if (z_size > 0) {
			z_buf = malloc(z_size);

			if (z_buf == NULL)
				return MALLOC_FAILED;
		}

		overflow = compress_stateless(in_buf, in_size, z_buf, &z_size, flush_type,
					      gzip_flag, level, hist_bits);

		if (overflow != COMPRESS_OUT_BUFFER_OVERFLOW) {
			if (overflow == 0)
				ret =
				    inflate_check(z_buf, z_size, in_buf, in_size, gzip_flag,
						  NULL, 0, hist_bits);

			if (overflow != 0 || ret != 0) {
				log_print("overflow error = %d\n", overflow);
				log_error(overflow);
				log_print("inflate ret = %d\n", ret);
				log_error(ret);

				log_print
				    ("Compressed array at level %d with gzip flag %d, flush type %d, and hist_bits %d: ",
				     level, gzip_flag, flush_type, hist_bits);

				log_uint8_t(z_buf, z_size);
				log_print("\n");
				log_print("Data: ");
				log_uint8_t(in_buf, in_size);

				printf("Failed on compress single pass overflow\n");
				print_error(ret);
				ret = OVERFLOW_TEST_ERROR;
			}
		}
	}

	print_error(ret);
	if (ret) {
		if (z_buf != NULL) {
			free(z_buf);
			z_buf = NULL;
		}
		if (in_buf != NULL)
			free(in_buf);
		return ret;
	}

	if (flush_type == FULL_FLUSH) {
		if (z_buf != NULL)
			free(z_buf);

		z_size = 2 * in_size + MAX_LOOPS * (hdr_bytes + 5);

		z_buf = malloc(z_size);

		if (z_buf == NULL)
			return MALLOC_FAILED;

		create_rand_repeat_data(z_buf, z_size);

		/* Else test valid flush type */
		ret = compress_stateless_full_flush(in_buf, in_size, z_buf, &z_size,
						    level, hist_bits);

		if (!ret)
			ret =
			    inflate_check(z_buf, z_size, in_buf, in_size, 0, NULL, 0,
					  hist_bits);
		else if (ret == COMPRESS_LOOP_COUNT_OVERFLOW)
			ret = 0;

		print_error(ret);

		if (ret) {
			log_print
			    ("Compressed array at level %d with gzip flag %d, flush type %d, and hist_bits %d: ",
			     level, gzip_flag, FULL_FLUSH, hist_bits);
			log_uint8_t(z_buf, z_size);
			log_print("\n");
			log_print("Data: ");
			log_uint8_t(in_buf, in_size);
		}
	}
	if (z_buf != NULL)
		free(z_buf);

	if (in_buf != NULL)
		free(in_buf);

	return ret;
}

/* Test deflate */
int test_compress(uint8_t * in_buf, uint32_t in_size, uint32_t flush_type)
{
	int ret = IGZIP_COMP_OK, fin_ret = IGZIP_COMP_OK;
	uint32_t overflow = 0, gzip_flag, level, hist_bits;
	uint32_t z_size = 0, z_size_max = 0, z_compressed_size, dict_len = 0;
	uint8_t *z_buf = NULL, *dict = NULL;

	/* Test a non overflow case */
	if (flush_type == NO_FLUSH)
		z_size_max = 2 * in_size + hdr_bytes + 2;
	else if (flush_type == SYNC_FLUSH || flush_type == FULL_FLUSH)
		z_size_max = 2 * in_size + MAX_LOOPS * (hdr_bytes + 5);
	else {
		printf("Invalid Flush Parameter\n");
		return COMPRESS_GENERAL_ERROR;
	}

	gzip_flag = rand() % 5;
	hist_bits = rand() % 16;
	level = get_rand_level();

	z_size = z_size_max;

	if (gzip_flag == IGZIP_GZIP)
		z_size += gzip_extra_bytes;
	else if (gzip_flag == IGZIP_GZIP_NO_HDR)
		z_size += gzip_trl_bytes;
	else if (gzip_flag == IGZIP_ZLIB)
		z_size += zlib_extra_bytes;
	else if (gzip_flag == IGZIP_ZLIB_NO_HDR)
		z_size += zlib_trl_bytes;

	z_buf = malloc(z_size);
	if (z_buf == NULL) {
		print_error(MALLOC_FAILED);
		return MALLOC_FAILED;
	}
	create_rand_repeat_data(z_buf, z_size);

	if (rand() % 8 == 0) {
		dict_len = (rand() % IGZIP_HIST_SIZE) + 1;
		dict = malloc(dict_len);
		if (dict == NULL) {
			print_error(MALLOC_FAILED);
			return MALLOC_FAILED;
		}
		create_rand_dict(dict, dict_len, z_buf, z_size);
	}

	ret = compress_single_pass(in_buf, in_size, z_buf, &z_size, flush_type,
				   gzip_flag, level, dict, dict_len, hist_bits);

	if (!ret)
		ret =
		    inflate_check(z_buf, z_size, in_buf, in_size, gzip_flag, dict, dict_len,
				  hist_bits);

	if (ret) {
		log_print
		    ("Compressed array at level %d with gzip flag %d, flush type %d, and hist_bits %d: ",
		     level, gzip_flag, flush_type, hist_bits);
		log_uint8_t(z_buf, z_size);
		log_print("\n");
		if (dict != NULL) {
			log_print("Using Dictionary: ");
			log_uint8_t(dict, dict_len);
			log_print("\n");
		}
		log_print("Data: ");
		log_uint8_t(in_buf, in_size);

		printf("Failed on compress single pass\n");
		print_error(ret);
	}

	if (dict != NULL) {
		free(dict);
		dict = NULL;
		dict_len = 0;
	}

	fin_ret |= ret;
	if (ret)
		goto test_compress_cleanup;

	z_compressed_size = z_size;
	z_size = z_size_max;
	create_rand_repeat_data(z_buf, z_size_max);

	if (rand() % 8 == 0) {
		dict_len = (rand() % IGZIP_HIST_SIZE) + 1;
		dict = malloc(dict_len);
		if (dict == NULL) {
			print_error(MALLOC_FAILED);
			return MALLOC_FAILED;
		}
		create_rand_dict(dict, dict_len, z_buf, z_size);
	}

	ret =
	    compress_multi_pass(in_buf, in_size, z_buf, &z_size, flush_type, gzip_flag, level,
				dict, dict_len, hist_bits);

	if (!ret)
		ret =
		    inflate_check(z_buf, z_size, in_buf, in_size, gzip_flag, dict, dict_len,
				  hist_bits);

	if (ret) {
		log_print
		    ("Compressed array at level %d with gzip flag %d, flush type %d and hist_bits %d: ",
		     level, gzip_flag, flush_type, hist_bits);
		log_uint8_t(z_buf, z_size);
		log_print("\n");
		if (dict != NULL) {
			log_print("Using Dictionary: ");
			log_uint8_t(dict, dict_len);
			log_print("\n");
		}
		log_print("Data: ");
		log_uint8_t(in_buf, in_size);

		printf("Failed on compress multi pass\n");
		print_error(ret);
	}

	if (dict != NULL) {
		free(dict);
		dict = NULL;
		dict_len = 0;
	}

	fin_ret |= ret;
	if (ret)
		goto test_compress_cleanup;

	ret = 0;

	/* Test random overflow case */
	if (flush_type == SYNC_FLUSH && z_compressed_size > in_size)
		z_compressed_size = in_size + 1;

	z_size = rand() % z_compressed_size;
	create_rand_repeat_data(z_buf, z_size);

	overflow = compress_single_pass(in_buf, in_size, z_buf, &z_size, flush_type,
					gzip_flag, level, dict, dict_len, hist_bits);

	if (overflow != COMPRESS_OUT_BUFFER_OVERFLOW) {
		if (overflow == 0)
			ret =
			    inflate_check(z_buf, z_size, in_buf, in_size, gzip_flag, dict,
					  dict_len, hist_bits);

		/* Rarely single pass overflow will compresses data
		 * better than the initial run. This is to stop that
		 * case from erroring. */
		if (overflow != 0 || ret != 0) {
			log_print("overflow error = %d\n", overflow);
			log_error(overflow);
			log_print("inflate ret = %d\n", ret);
			log_error(ret);

			log_print
			    ("Compressed array at level %d with gzip flag %d, flush type %d, and hist_bits %d: ",
			     level, gzip_flag, flush_type, hist_bits);
			log_uint8_t(z_buf, z_size);
			log_print("\n");
			log_print("Data: ");
			log_uint8_t(in_buf, in_size);

			printf("Failed on compress single pass overflow\n");
			print_error(ret);
			ret = OVERFLOW_TEST_ERROR;
		}
	}

	fin_ret |= ret;
	if (ret)
		goto test_compress_cleanup;

	if (flush_type == NO_FLUSH) {
		create_rand_repeat_data(z_buf, z_size);

		overflow =
		    compress_multi_pass(in_buf, in_size, z_buf, &z_size, flush_type,
					gzip_flag, level, dict, dict_len, hist_bits);

		if (overflow != COMPRESS_OUT_BUFFER_OVERFLOW) {
			if (overflow == 0)
				ret =
				    inflate_check(z_buf, z_size, in_buf, in_size, gzip_flag,
						  dict, dict_len, hist_bits);

			/* Rarely multi pass overflow will compresses data
			 * better than the initial run. This is to stop that
			 * case from erroring */
			if (overflow != 0 || ret != 0) {
				log_print("overflow error = %d\n", overflow);
				log_error(overflow);
				log_print("inflate ret = %d\n", ret);
				log_error(ret);
				log_print
				    ("Compressed array at level %d with gzip flag %d, flush type %d, and hist_bits %d: ",
				     level, gzip_flag, flush_type, hist_bits);
				log_uint8_t(z_buf, z_size);
				log_print("\n");
				log_print("Data: ");
				log_uint8_t(in_buf, in_size);

				printf("Failed on compress multi pass overflow\n");
				print_error(ret);
				ret = OVERFLOW_TEST_ERROR;
			}
		}
		fin_ret |= ret;
	}

      test_compress_cleanup:
	free(z_buf);

	return fin_ret;
}

/* Test swapping flush types in the middle of compression */
int test_flush(uint8_t * in_buf, uint32_t in_size)
{
	int fin_ret = IGZIP_COMP_OK, ret;
	uint32_t z_size, flush_type = 0, gzip_flag, level;
	uint8_t *z_buf = NULL;

	gzip_flag = rand() % 5;
	level = get_rand_level();

	z_size = 2 * in_size + 2 * hdr_bytes + 8;
	if (gzip_flag == IGZIP_GZIP)
		z_size += gzip_extra_bytes;
	else if (gzip_flag == IGZIP_GZIP_NO_HDR)
		z_size += gzip_trl_bytes;
	else if (gzip_flag == IGZIP_ZLIB)
		z_size += zlib_extra_bytes;
	else if (gzip_flag == IGZIP_ZLIB_NO_HDR)
		z_size += zlib_trl_bytes;

	z_buf = malloc(z_size);

	if (z_buf == NULL)
		return MALLOC_FAILED;

	create_rand_repeat_data(z_buf, z_size);

	while (flush_type < 3)
		flush_type = rand() & 0xFFFF;

	/* Test invalid flush */
	ret = compress_single_pass(in_buf, in_size, z_buf, &z_size, flush_type,
				   gzip_flag, level, NULL, 0, 0);

	if (ret == COMPRESS_GENERAL_ERROR)
		ret = 0;
	else {
		printf("Failed when passing invalid flush parameter\n");
		ret = INVALID_FLUSH_ERROR;
	}

	fin_ret |= ret;
	print_error(ret);

	create_rand_repeat_data(z_buf, z_size);

	/* Test swapping flush type */
	ret =
	    compress_swap_flush(in_buf, in_size, z_buf, &z_size, rand() % 3, level, gzip_flag);

	if (!ret)
		ret = inflate_check(z_buf, z_size, in_buf, in_size, gzip_flag, NULL, 0, 0);

	if (ret) {
		log_print("Compressed array at level %d with gzip flag %d: ", level,
			  gzip_flag);
		log_uint8_t(z_buf, z_size);
		log_print("\n");
		log_print("Data: ");
		log_uint8_t(in_buf, in_size);

		printf("Failed on swapping flush type\n");
		print_error(ret);
	}

	fin_ret |= ret;
	print_error(ret);

	return fin_ret;
}

/* Test there are no length distance pairs across full flushes */
int test_full_flush(uint8_t * in_buf, uint32_t in_size)
{
	int ret = IGZIP_COMP_OK;
	uint32_t z_size, gzip_flag, level;
	uint8_t *z_buf = NULL;

	gzip_flag = rand() % 5;
	level = get_rand_level();
	z_size = 2 * in_size + MAX_LOOPS * (hdr_bytes + 5);

	if (gzip_flag == IGZIP_GZIP)
		z_size += gzip_extra_bytes;
	else if (gzip_flag == IGZIP_GZIP_NO_HDR)
		z_size += gzip_trl_bytes;
	else if (gzip_flag == IGZIP_ZLIB)
		z_size += zlib_extra_bytes;
	else if (gzip_flag == IGZIP_ZLIB_NO_HDR)
		z_size += zlib_trl_bytes;

	z_buf = malloc(z_size);
	if (z_buf == NULL) {
		print_error(MALLOC_FAILED);
		return MALLOC_FAILED;
	}

	create_rand_repeat_data(z_buf, z_size);

	ret = compress_full_flush(in_buf, in_size, z_buf, &z_size, gzip_flag, level);

	if (!ret)
		ret = inflate_check(z_buf, z_size, in_buf, in_size, gzip_flag, NULL, 0, 0);

	if (ret) {
		log_print("Compressed array at level %d with gzip flag %d and flush type %d: ",
			  level, gzip_flag, FULL_FLUSH);
		log_uint8_t(z_buf, z_size);
		log_print("\n");
		log_print("Data: ");
		log_uint8_t(in_buf, in_size);

		printf("Failed on compress multi pass\n");
		print_error(ret);
	}

	free(z_buf);

	return ret;
}

int test_inflate(struct vect_result *in_vector)
{
	int ret = IGZIP_COMP_OK;
	uint8_t *compress_buf = in_vector->vector, *out_buf = NULL;
	uint64_t compress_len = in_vector->vector_length;
	uint32_t out_size = 0;

	out_size = 10 * in_vector->vector_length;
	out_buf = malloc(out_size);
	if (out_buf == NULL)
		return MALLOC_FAILED;

	ret = inflate_stateless_pass(compress_buf, compress_len, out_buf, &out_size, 0);

	if (ret == INFLATE_LEFTOVER_INPUT)
		ret = ISAL_DECOMP_OK;

	if (ret != in_vector->expected_error)
		printf("Inflate return value incorrect, %d != %d\n", ret,
		       in_vector->expected_error);
	else
		ret = IGZIP_COMP_OK;

	if (!ret) {
		ret = inflate_multi_pass(compress_buf, compress_len, out_buf, &out_size,
					 0, NULL, 0, 0);

		if (ret == INFLATE_LEFTOVER_INPUT)
			ret = ISAL_DECOMP_OK;

		if (ret != in_vector->expected_error)
			printf("Inflate return value incorrect, %d != %d\n", ret,
			       in_vector->expected_error);
		else
			ret = IGZIP_COMP_OK;
	}

	return ret;

}

int test_large(uint8_t * in_buf, uint32_t in_size, uint64_t large_size)
{

	int ret = IGZIP_COMP_OK;
	uint32_t gzip_flag, level;
	uint32_t z_size = 0, z_size_max = 0, tmp_buf_size;
	uint8_t *z_buf = NULL, *tmp_buf = NULL;
	int flush_type = NO_FLUSH;

	/* Test a non overflow case */
	z_size_max = MAX_LARGE_COMP_BUF_SIZE;

	gzip_flag = rand() % 5;
	level = get_rand_level();

	z_size = z_size_max;
	z_buf = malloc(z_size);
	if (z_buf == NULL) {
		print_error(MALLOC_FAILED);
		return MALLOC_FAILED;
	}
	create_rand_repeat_data(z_buf, z_size);

	tmp_buf_size = IBUF_SIZE;
	tmp_buf = malloc(tmp_buf_size);
	if (tmp_buf == NULL) {
		print_error(MALLOC_FAILED);
		return MALLOC_FAILED;
	}

	ret =
	    compress_ver_rep_buf(in_buf, in_size, large_size, z_buf, z_size, tmp_buf,
				 tmp_buf_size, flush_type, gzip_flag, level);

	if (ret)
		print_error(ret);

	if (z_buf != NULL) {
		free(z_buf);
		z_buf = NULL;
	}

	if (tmp_buf != NULL) {
		free(tmp_buf);
		tmp_buf = NULL;
	}

	return ret;
}

/* Run multiple compression tests on data stored in a file */
int test_compress_file(char *file_name)
{
	int ret = IGZIP_COMP_OK;
	uint64_t in_size;
	uint8_t *in_buf = NULL;
	FILE *in_file = NULL;

	in_file = fopen(file_name, "rb");
	if (!in_file) {
		printf("Failed to open file %s\n", file_name);
		return FILE_READ_FAILED;
	}

	in_size = get_filesize(in_file);
	if (in_size > MAX_FILE_SIZE)
		in_size = MAX_FILE_SIZE;

	if (in_size != 0) {
		in_buf = malloc(in_size);
		if (in_buf == NULL) {
			printf("Failed to allocate in_buf for test_compress_file\n");
			return MALLOC_FAILED;
		}
		if (fread(in_buf, 1, in_size, in_file) != in_size) {
			printf("Failed to read in_buf from test_compress_file\n");
			free(in_buf);
			return FILE_READ_FAILED;
		}
	}

	ret |= test_compress_stateless(in_buf, in_size, NO_FLUSH);
	if (!ret)
		ret |= test_compress_stateless(in_buf, in_size, SYNC_FLUSH);
	if (!ret)
		ret |= test_compress_stateless(in_buf, in_size, FULL_FLUSH);
	if (!ret)
		ret |= test_compress(in_buf, in_size, NO_FLUSH);
	if (!ret)
		ret |= test_compress(in_buf, in_size, SYNC_FLUSH);
	if (!ret)
		ret |= test_compress(in_buf, in_size, FULL_FLUSH);
	if (!ret)
		ret |= test_flush(in_buf, in_size);

	if (ret)
		printf("Failed on file %s\n", file_name);

	if (in_buf != NULL)
		free(in_buf);

	return ret;
}

int create_custom_hufftables(struct isal_hufftables *hufftables_custom, int file_count,
			     char *files[])
{
	long int file_length;
	uint8_t *stream = NULL;
	struct isal_huff_histogram histogram;
	FILE *file;
	int i;

	memset(&histogram, 0, sizeof(histogram));

	for (i = 0; i < file_count; i++) {
		printf("Processing %s\n", files[i]);
		file = fopen(files[i], "r");
		if (file == NULL) {
			printf("Error opening file\n");
			return 1;
		}
		fseek(file, 0, SEEK_END);
		file_length = ftell(file);
		fseek(file, 0, SEEK_SET);
		file_length -= ftell(file);

		if (file_length > 0) {
			stream = malloc(file_length);
			if (stream == NULL) {
				printf("Failed to allocate memory to read in file\n");
				fclose(file);
				return 1;
			}
		}

		if (fread(stream, 1, file_length, file) != file_length) {
			printf("Error occurred when reading file\n");
			fclose(file);
			free(stream);
			stream = NULL;
			return 1;
		}

		/* Create a histogram of frequency of symbols found in stream to
		 * generate the huffman tree.*/
		isal_update_histogram(stream, file_length, &histogram);

		fclose(file);
		if (stream != NULL) {
			free(stream);
			stream = NULL;
		}
	}

	return isal_create_hufftables(hufftables_custom, &histogram);

}

int main(int argc, char *argv[])
{
	int i = 0, j = 0, ret = 0, fin_ret = 0;
	uint32_t in_size = 0, offset = 0;
	uint8_t *in_buf;
	struct isal_hufftables hufftables_custom, hufftables_sub;
	uint64_t iterations, large_buf_size;
	size_t argv_index;
	char **input_files;
	size_t file_count;

	argv_index = parse_options(argc, argv);

	input_files = &argv[argv_index];
	file_count = argc - argv_index;

	if (options.verbose)
		setbuf(stdout, NULL);

	printf("Window Size: %d K\n", IGZIP_HIST_SIZE / 1024);
	printf("Test Seed  : %d\n", options.test_seed);
	printf("Randoms    : %d\n", options.randoms);
	srand(options.test_seed);

	hufftables_subset = &hufftables_sub;
	if (file_count > 0) {
		ret = create_custom_hufftables(&hufftables_custom, file_count, input_files);
		if (ret == 0)
			hufftables = &hufftables_custom;
		else {
			printf("Failed to generate custom hufftable");
			return -1;
		}
	}

	in_buf = malloc(IBUF_SIZE);
	memset(in_buf, 0, IBUF_SIZE);

	if (in_buf == NULL) {
		fprintf(stderr, "Can't allocate in_buf memory\n");
		return -1;
	}

	if (file_count > 0) {
		printf("igzip_rand_test files:                  ");

		for (i = 0; i < file_count; i++) {
			ret |= test_compress_file(input_files[i]);
			if (ret)
				return ret;
		}

		printf("................");
		printf("%s\n", ret ? "Fail" : "Pass");
		fin_ret |= ret;
	}

	printf("igzip_rand_test stateless:              ");

	ret = test_compress_stateless((uint8_t *) str1, sizeof(str1), NO_FLUSH);
	if (ret)
		return ret;

	ret |= test_compress_stateless((uint8_t *) str2, sizeof(str2), NO_FLUSH);
	if (ret)
		return ret;

	for (i = 0; i < options.randoms; i++) {
		in_size = get_rand_data_length();
		offset = rand() % (IBUF_SIZE + 1 - in_size);
		in_buf += offset;

		create_rand_repeat_data(in_buf, in_size);

		ret |= test_compress_stateless(in_buf, in_size, NO_FLUSH);

		in_buf -= offset;

		if (i % (options.randoms / 16) == 0)
			printf(".");

		if (ret)
			return ret;
	}

	for (i = 0; i < options.randoms / 16; i++) {
		create_rand_repeat_data(in_buf, PAGE_SIZE);
		ret |= test_compress_stateless(in_buf, PAGE_SIZE, NO_FLUSH);	// good for efence
		if (ret)
			return ret;
	}

	fin_ret |= ret;

	ret = test_compress_stateless((uint8_t *) str1, sizeof(str1), SYNC_FLUSH);
	if (ret)
		return ret;

	ret |= test_compress_stateless((uint8_t *) str2, sizeof(str2), SYNC_FLUSH);
	if (ret)
		return ret;

	for (i = 0; i < 16; i++) {
		in_size = get_rand_data_length();
		offset = rand() % (IBUF_SIZE + 1 - in_size);
		in_buf += offset;

		create_rand_repeat_data(in_buf, in_size);

		ret |= test_compress_stateless(in_buf, in_size, SYNC_FLUSH);

		in_buf -= offset;

		if (ret)
			return ret;
	}

	fin_ret |= ret;

	printf("%s\n", ret ? "Fail" : "Pass");

	printf("igzip_rand_test stateless FULL_FLUSH:   ");

	ret = test_compress_stateless((uint8_t *) str1, sizeof(str1), FULL_FLUSH);
	if (ret)
		return ret;

	ret |= test_compress_stateless((uint8_t *) str2, sizeof(str2), FULL_FLUSH);
	if (ret)
		return ret;

	for (i = 0; i < options.randoms; i++) {
		in_size = get_rand_data_length();
		offset = rand() % (IBUF_SIZE + 1 - in_size);
		in_buf += offset;

		create_rand_repeat_data(in_buf, in_size);

		ret |= test_compress_stateless(in_buf, in_size, FULL_FLUSH);

		in_buf -= offset;

		if (i % (options.randoms / 16) == 0)
			printf(".");

		if (ret)
			return ret;
	}

	for (i = 0; i < options.randoms / 16; i++) {
		create_rand_repeat_data(in_buf, PAGE_SIZE);
		ret |= test_compress_stateless(in_buf, PAGE_SIZE, FULL_FLUSH);	// good for efence
		if (ret)
			return ret;
	}
	fin_ret |= ret;

	printf("%s\n", ret ? "Fail" : "Pass");

	printf("igzip_rand_test stateful  NO_FLUSH:     ");

	memcpy(in_buf, str1, sizeof(str1));
	ret = test_compress(in_buf, sizeof(str1), NO_FLUSH);
	if (ret)
		return ret;

	memcpy(in_buf, str2, sizeof(str2));
	ret |= test_compress(in_buf, sizeof(str2), NO_FLUSH);
	if (ret)
		return ret;

	for (i = 0; i < options.randoms; i++) {
		in_size = get_rand_data_length();
		offset = rand() % (IBUF_SIZE + 1 - in_size);
		in_buf += offset;

		create_rand_repeat_data(in_buf, in_size);

		ret |= test_compress(in_buf, in_size, NO_FLUSH);

		in_buf -= offset;

		if (i % (options.randoms / 16) == 0)
			printf(".");
		if (ret)
			return ret;
	}

	fin_ret |= ret;

	printf("%s\n", ret ? "Fail" : "Pass");

	printf("igzip_rand_test stateful  SYNC_FLUSH:   ");

	memcpy(in_buf, str1, sizeof(str1));
	ret = test_compress(in_buf, sizeof(str1), SYNC_FLUSH);
	if (ret)
		return ret;

	memcpy(in_buf, str2, sizeof(str2));
	ret |= test_compress(in_buf, sizeof(str2), SYNC_FLUSH);
	if (ret)
		return ret;

	for (i = 0; i < options.randoms; i++) {
		in_size = get_rand_data_length();
		offset = rand() % (IBUF_SIZE + 1 - in_size);
		in_buf += offset;

		create_rand_repeat_data(in_buf, in_size);

		ret |= test_compress(in_buf, in_size, SYNC_FLUSH);

		in_buf -= offset;

		if (i % (options.randoms / 16) == 0)
			printf(".");
		if (ret)
			return ret;
	}

	fin_ret |= ret;

	printf("%s\n", ret ? "Fail" : "Pass");

	printf("igzip_rand_test stateful  FULL_FLUSH:   ");

	memcpy(in_buf, str1, sizeof(str1));
	ret = test_compress(in_buf, sizeof(str1), FULL_FLUSH);
	if (ret)
		return ret;

	memcpy(in_buf, str2, sizeof(str2));
	ret |= test_compress(in_buf, sizeof(str2), FULL_FLUSH);
	if (ret)
		return ret;

	for (i = 0; i < options.randoms; i++) {
		in_size = get_rand_data_length();
		offset = rand() % (IBUF_SIZE + 1 - in_size);
		in_buf += offset;

		create_rand_repeat_data(in_buf, in_size);

		ret |= test_compress(in_buf, in_size, FULL_FLUSH);

		in_buf -= offset;

		if (i % (options.randoms / 16) == 0)
			printf(".");
		if (ret)
			return ret;
	}

	for (i = 0; i < options.randoms / 8; i++) {
		in_size = get_rand_data_length();
		offset = rand() % (IBUF_SIZE + 1 - in_size);
		in_buf += offset;

		create_rand_repeat_data(in_buf, in_size);

		ret |= test_full_flush(in_buf, in_size);

		in_buf -= offset;

		if (ret)
			return ret;
	}

	fin_ret |= ret;

	printf("%s\n", ret ? "Fail" : "Pass");

	printf("igzip_rand_test stateful  Change Flush: ");

	ret = test_flush((uint8_t *) str1, sizeof(str1));
	if (ret)
		return ret;

	ret |= test_flush((uint8_t *) str2, sizeof(str2));
	if (ret)
		return ret;

	for (i = 0; i < options.randoms / 4; i++) {
		in_size = get_rand_data_length();
		offset = rand() % (IBUF_SIZE + 1 - in_size);
		in_buf += offset;

		create_rand_repeat_data(in_buf, in_size);

		ret |= test_flush(in_buf, in_size);

		in_buf -= offset;

		if (i % ((options.randoms / 4) / 16) == 0)
			printf(".");
		if (ret)
			return ret;
	}

	fin_ret |= ret;

	printf("%s\n", ret ? "Fail" : "Pass");

	if (options.do_large_test) {
		printf("igzip_rand_test large input             ");

		iterations = options.randoms / 256 + 1;
		for (i = 0; i < iterations; i++) {
			in_size = rand() % (32 * 1024) + 16 * 1024;
			offset = rand() % (IBUF_SIZE + 1 - in_size);
			in_buf += offset;

			large_buf_size = 1;
			large_buf_size <<= 32;
			large_buf_size += rand() % (1024 * 1024) + 1;
			create_rand_repeat_data(in_buf, in_size);

			ret |= test_large(in_buf, in_size, large_buf_size);

			if (ret)
				return ret;

			in_buf -= offset;

			if (iterations < 16) {
				for (j = 0; j < 16 / iterations; j++)
					printf(".");
			} else if (i % (iterations / 16) == 0)
				printf(".");

		}

		if (iterations < 16) {
			for (j = (16 / iterations) * iterations; j < 16; j++)
				printf(".");
		}

		printf("%s\n", ret ? "Fail" : "Pass");
	}

	printf("igzip_rand_test inflate   Std Vectors:  ");

	for (i = 0; i < sizeof(std_vect_array) / sizeof(struct vect_result); i++) {
		ret = test_inflate(&std_vect_array[i]);
		if (ret)
			return ret;
	}
	printf("................");
	printf("%s\n", ret ? "Fail" : "Pass");

	printf("igzip rand test finished: %s\n",
	       fin_ret ? "Some tests failed" : "All tests passed");

	return fin_ret != IGZIP_COMP_OK;
}
