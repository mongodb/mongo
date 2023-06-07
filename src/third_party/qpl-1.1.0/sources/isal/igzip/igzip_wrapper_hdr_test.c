/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "checksum_test_ref.h"
#include "igzip_lib.h"
#include "igzip_wrapper.h"

#ifndef TEST_SEED
# define TEST_SEED 0x1234
#endif
#ifndef RANDOMS
# define RANDOMS 0x4000
#endif

#define EXTRA_SIZE_MAX 256
#define NAME_SIZE_MAX 256
#define COMMENT_SIZE_MAX 1024

#define EXTRA_SIZE 10
#define NAME_SIZE 25
#define COMMENT_SIZE 192

enum {
	INVALID_WRAPPER = ISAL_INVALID_WRAPPER,
	UNSUPPORTED_METHOD = ISAL_UNSUPPORTED_METHOD,
	INCORRECT_CHECKSUM = ISAL_INCORRECT_CHECKSUM,
	NO_ERROR = ISAL_DECOMP_OK,
	END_INPUT = ISAL_END_INPUT,
	NAME_OVERFLOW = ISAL_NAME_OVERFLOW,
	COMMENT_OVERFLOW = ISAL_COMMENT_OVERFLOW,
	EXTRA_OVERFLOW = ISAL_EXTRA_OVERFLOW,
	INCORRECT_TEXT_FLAG,
	INCORRECT_TIME,
	INCORRECT_XFLAGS,
	INCORRECT_OS,
	INCORRECT_EXTRA_LEN,
	INCORRECT_EXTRA_BUF,
	INCORRECT_NAME,
	INCORRECT_COMMENT,
	INCORRECT_INFO,
	INCORRECT_LEVEL,
	INCORRECT_DICT_FLAG,
	INCORRECT_DICT_ID,
	INCORRECT_HDR_LEN,
	INSUFFICIENT_BUFFER_SIZE,
	INCORRECT_WRITE_RETURN,
	MALLOC_FAILED
};

void print_error(int32_t error)
{
	printf("Error Code %d: ", error);
	switch (error) {
	case END_INPUT:
		printf("End of input reached before header decompressed\n");
		break;
	case INVALID_WRAPPER:
		printf("Invalid gzip wrapper found\n");
		break;
	case UNSUPPORTED_METHOD:
		printf("Unsupported decompression method found\n");
		break;
	case INCORRECT_CHECKSUM:
		printf("Incorrect header checksum found\n");
		break;
	case NAME_OVERFLOW:
		printf("Name buffer overflow while decompression\n");
		break;
	case COMMENT_OVERFLOW:
		printf("Comment buffer overflow while decompressing\n");
	case EXTRA_OVERFLOW:
		printf("Extra buffer overflow while decomrpessiong\n");
		break;
	case INCORRECT_TEXT_FLAG:
		printf("Incorrect text field found\n");
		break;
	case INCORRECT_TIME:
		printf("Incorrect time filed found\n");
		break;
	case INCORRECT_XFLAGS:
		printf("Incorrect xflags field found\n");
		break;
	case INCORRECT_OS:
		printf("Incorrect os field found\n");
		break;
	case INCORRECT_EXTRA_LEN:
		printf("Incorect extra_len field found\n");
		break;
	case INCORRECT_EXTRA_BUF:
		printf("Incorrect extra buffer found\n");
		break;
	case INCORRECT_NAME:
		printf("Incorrect name found\n");
		break;
	case INCORRECT_COMMENT:
		printf("Incorrect comment found\n");
		break;
	case INCORRECT_INFO:
		printf("Incorrect info found\n");
		break;
	case INCORRECT_LEVEL:
		printf("Incorrect level found\n");
		break;
	case INCORRECT_DICT_FLAG:
		printf("Incorrect dictionary flag found\n");
		break;
	case INCORRECT_DICT_ID:
		printf("Incorrect dictionary id found\n");
		break;
	case INCORRECT_HDR_LEN:
		printf("Incorrect header length found\n");
		break;
	case INSUFFICIENT_BUFFER_SIZE:
		printf("Insufficient buffer size to write header\n");
		break;
	case INCORRECT_WRITE_RETURN:
		printf("Header write returned an incorrect value\n");
		break;
	case MALLOC_FAILED:
		printf("Failed to allocate buffers\n");
		break;
	case NO_ERROR:
		printf("No error found\n");
	}
}

void print_uint8_t(uint8_t * array, uint64_t length, char *prepend)
{
	const int line_size = 16;
	int i;

	if (array == NULL)
		printf("%s(NULL)", prepend);
	else if (length == 0)
		printf("%s(Empty)", prepend);

	for (i = 0; i < length; i++) {
		if (i == 0)
			printf("%s0x%04x\t", prepend, i);
		else if ((i % line_size) == 0)
			printf("\n%s0x%04x\t", prepend, i);
		else
			printf(" ");
		printf("0x%02x,", array[i]);
	}
	printf("\n");
}

void print_string(char *str, uint32_t str_max_len, char *prepend)
{
	const int line_size = 64;
	uint32_t i = 0;

	while (str[i] != 0 && i < str_max_len) {
		if (i == 0)
			printf("%s0x%04x\t", prepend, i);
		else if ((i % line_size) == 0)
			printf("\n%s0x%04x\t", prepend, i);

		printf("%c", str[i]);
		i++;
	}
	printf("\n");
}

void print_gzip_header(struct isal_gzip_header *gz_hdr, char *prepend1, char *prepend2)
{
	printf("%sText: %d, Time: 0x%08x, Xflags: 0x%x, OS: 0x%x\n", prepend1,
	       gz_hdr->text, gz_hdr->time, gz_hdr->xflags, gz_hdr->os);

	printf("%sExtra:  Extra_len = 0x%x\n", prepend1, gz_hdr->extra_len);
	if (gz_hdr->extra_len < EXTRA_SIZE_MAX)
		print_uint8_t(gz_hdr->extra, gz_hdr->extra_len, prepend2);
	else
		printf("%sExtra field larger than EXTRA_SIZE_MAX\n", prepend2);

	printf("%sName:\n", prepend1);
	if (gz_hdr->name_buf_len < NAME_SIZE_MAX)
		print_string(gz_hdr->name, gz_hdr->name_buf_len, prepend2);
	else
		printf("%sName field larger than NAME_SIZE_MAX\n", prepend2);

	printf("%sComment:\n", prepend1);
	if (gz_hdr->comment_buf_len < COMMENT_SIZE_MAX)
		print_string(gz_hdr->comment, gz_hdr->comment_buf_len, prepend2);
	else
		printf("%sComment field larger than COMMENT_SIZE_MAX\n", prepend2);
}

void print_zlib_header(struct isal_zlib_header *z_hdr, char *prepend)
{
	printf("%sInfo: 0x%x\n", prepend, z_hdr->info);
	printf("%sLevel: 0x%x\n", prepend, z_hdr->level);
	printf("%sDictionary: Flag = 0x%x, Id =0x%x\n", prepend, z_hdr->dict_flag,
	       z_hdr->dict_id);
}

void print_gzip_final_verbose(uint8_t * hdr_buf, uint32_t hdr_buf_len,
			      struct isal_gzip_header *gz_hdr_orig,
			      struct isal_gzip_header *gz_hdr)
{
#ifdef VERBOSE
	printf("\n");
	if (gz_hdr_orig != NULL) {
		printf("Original Gzip Header Struct:\n");
		print_gzip_header(gz_hdr_orig, "\t", "\t\t");
		printf("\n");
	}

	if (gz_hdr != NULL) {
		printf("Parsed Gzip Header Struct:\n");
		print_gzip_header(gz_hdr, "\t", "\t\t");
		printf("\n");
	}

	if (hdr_buf != NULL) {
		printf("Serialized Gzip Header:\n");
		print_uint8_t(hdr_buf, hdr_buf_len, "\t");
		printf("\n");
	}
#endif
	return;
}

void print_zlib_final_verbose(uint8_t * hdr_buf, uint32_t hdr_buf_len,
			      struct isal_zlib_header *z_hdr_orig,
			      struct isal_zlib_header *z_hdr)
{
#ifdef VERBOSE
	printf("\n");
	if (z_hdr_orig != NULL) {
		printf("Original Zlib Header Struct:\n");
		print_zlib_header(z_hdr_orig, "\t");
		printf("\n");
	}

	if (z_hdr != NULL) {
		printf("Parsed Zlib Header Struct:\n");
		print_zlib_header(z_hdr, "\t");
		printf("\n");
	}

	if (hdr_buf != NULL) {
		printf("Serialized Zlib Header:\n");
		print_uint8_t(hdr_buf, hdr_buf_len, "\t");
		printf("\n");
	}
#endif
	return;
}

int gzip_header_size(struct isal_gzip_header *gz_hdr)
{
	int hdr_size = 10;
	if (gz_hdr->extra != NULL) {
		hdr_size += 2 + gz_hdr->extra_len;
	}
	if (gz_hdr->name != NULL) {
		hdr_size += strnlen(gz_hdr->name, gz_hdr->name_buf_len) + 1;
	}
	if (gz_hdr->comment != NULL) {
		hdr_size += strnlen(gz_hdr->comment, gz_hdr->comment_buf_len) + 1;
	}

	if (gz_hdr->hcrc) {
		hdr_size += 2;
	}

	return hdr_size;
}

int zlib_header_size(struct isal_zlib_header *z_hdr)
{
	if (z_hdr->dict_flag)
		return 6;
	else
		return 2;
}

void rand_string(char *string, uint32_t str_len)
{
	int i;

	if (str_len == 0 || string == NULL)
		return;
	for (i = 0; i < str_len - 1; i++) {
		string[i] = rand() % 26 + 65;
	}
	string[str_len - 1] = 0;
}

void rand_buf(uint8_t * buf, uint32_t buf_len)
{
	int i;

	if (buf_len == 0 || buf == NULL)
		return;

	for (i = 0; i < buf_len; i++) {
		buf[i] = rand();
	}
}

int malloc_gzip_header(struct isal_gzip_header *gz_hdr)
{
	gz_hdr->extra = NULL;
	if (gz_hdr->extra_buf_len) {
		gz_hdr->extra = malloc(gz_hdr->extra_buf_len);
		if (gz_hdr->extra == NULL)
			return MALLOC_FAILED;
	}

	gz_hdr->name = NULL;
	if (gz_hdr->name_buf_len) {
		gz_hdr->name = malloc(gz_hdr->name_buf_len);
		if (gz_hdr->name == NULL)
			return MALLOC_FAILED;
	}

	gz_hdr->comment = NULL;
	if (gz_hdr->comment_buf_len) {
		gz_hdr->comment = malloc(gz_hdr->comment_buf_len);
		if (gz_hdr->comment == NULL)
			return MALLOC_FAILED;
	}

	return 0;
}

void free_gzip_header(struct isal_gzip_header *gz_hdr)
{
	if (gz_hdr->extra != NULL) {
		free(gz_hdr->extra);
		gz_hdr->extra = NULL;
	}

	if (gz_hdr->name != NULL) {
		free(gz_hdr->name);
		gz_hdr->name = NULL;
	}

	if (gz_hdr->comment != NULL) {
		free(gz_hdr->comment);
		gz_hdr->comment = NULL;
	}

}

int gen_rand_gzip_header(struct isal_gzip_header *gz_hdr)
{
	int ret = 0;
	int field_set_space = 8;

	isal_gzip_header_init(gz_hdr);

	if (rand() % field_set_space != 0)
		gz_hdr->text = rand() % 2;
	if (rand() % field_set_space != 0)
		gz_hdr->time = rand();
	if (rand() % field_set_space != 0)
		gz_hdr->xflags = rand() % 256;
	if (rand() % field_set_space != 0)
		gz_hdr->os = rand() % 256;

	if (rand() % field_set_space != 0) {
		gz_hdr->extra_buf_len = rand() % EXTRA_SIZE_MAX;
		gz_hdr->extra_len = gz_hdr->extra_buf_len;
	}

	if (rand() % field_set_space != 0)
		gz_hdr->name_buf_len = rand() % NAME_SIZE_MAX;

	if (rand() % field_set_space != 0)
		gz_hdr->comment_buf_len = rand() % COMMENT_SIZE_MAX;

	gz_hdr->hcrc = rand() % 2;

	ret = malloc_gzip_header(gz_hdr);
	if (ret)
		return ret;

	rand_buf(gz_hdr->extra, gz_hdr->extra_len);
	rand_string(gz_hdr->name, gz_hdr->name_buf_len);
	rand_string(gz_hdr->comment, gz_hdr->comment_buf_len);

	return ret;
}

void gen_rand_zlib_header(struct isal_zlib_header *z_hdr)
{
	z_hdr->info = rand() % 16;
	z_hdr->level = rand() % 4;
	z_hdr->dict_flag = rand() % 2;
	z_hdr->dict_id = rand();
}

int write_gzip_header(uint8_t * hdr_buf, uint32_t hdr_buf_len, struct isal_gzip_header *gz_hdr)
{

	struct isal_zstream stream;
	uint32_t hdr_len = gzip_header_size(gz_hdr);
	uint32_t len;

	isal_deflate_init(&stream);
	stream.next_out = hdr_buf;
	stream.avail_out = rand() % hdr_len;
	len = isal_write_gzip_header(&stream, gz_hdr);

	if (len != hdr_len) {
		printf("len = %d, hdr_buf_len = %d\n", len, hdr_len);
		print_gzip_final_verbose(hdr_buf, hdr_buf_len, gz_hdr, NULL);
		print_error(INCORRECT_HDR_LEN);
		return INCORRECT_HDR_LEN;
	}

	if (hdr_buf_len < hdr_len) {
		print_gzip_final_verbose(NULL, 0, gz_hdr, NULL);
		print_error(INSUFFICIENT_BUFFER_SIZE);
		return INSUFFICIENT_BUFFER_SIZE;
	}

	stream.avail_out = hdr_buf_len;

	len = isal_write_gzip_header(&stream, gz_hdr);

	if (len) {
		print_gzip_final_verbose(hdr_buf, hdr_buf_len, gz_hdr, NULL);
		print_error(INCORRECT_WRITE_RETURN);
		return INCORRECT_WRITE_RETURN;;
	}

	return 0;
}

int write_zlib_header(uint8_t * hdr_buf, uint32_t hdr_buf_len, struct isal_zlib_header *z_hdr)
{
	struct isal_zstream stream;
	uint32_t hdr_len = zlib_header_size(z_hdr);
	uint32_t len;

	isal_deflate_init(&stream);
	stream.next_out = hdr_buf;
	stream.avail_out = rand() % hdr_len;
	len = isal_write_zlib_header(&stream, z_hdr);

	if (len != hdr_len) {
		print_zlib_final_verbose(hdr_buf, hdr_buf_len, z_hdr, NULL);
		print_error(INCORRECT_HDR_LEN);
		return INCORRECT_HDR_LEN;
	}

	if (hdr_buf_len < hdr_len) {
		print_zlib_final_verbose(NULL, 0, z_hdr, NULL);
		print_error(INSUFFICIENT_BUFFER_SIZE);
		return INSUFFICIENT_BUFFER_SIZE;
	}

	stream.avail_out = hdr_buf_len;

	len = isal_write_zlib_header(&stream, z_hdr);

	if (len) {
		print_zlib_final_verbose(hdr_buf, hdr_buf_len, z_hdr, NULL);
		print_error(INCORRECT_WRITE_RETURN);
		return INCORRECT_WRITE_RETURN;;
	}

	return 0;
}

int compare_gzip_headers(struct isal_gzip_header *gz_hdr1, struct isal_gzip_header *gz_hdr2)
{
	int ret = 0;
	uint32_t max_len;

	if (gz_hdr1->text != gz_hdr2->text)
		return INCORRECT_TEXT_FLAG;

	if (gz_hdr1->time != gz_hdr2->time)
		return INCORRECT_TIME;

	if (gz_hdr1->xflags != gz_hdr2->xflags)
		return INCORRECT_XFLAGS;

	if (gz_hdr1->os != gz_hdr2->os)
		return INCORRECT_OS;

	if (gz_hdr1->extra_len != gz_hdr2->extra_len)
		return INCORRECT_EXTRA_LEN;

	if (gz_hdr1->extra != NULL && gz_hdr2->extra != NULL) {
		ret = memcmp(gz_hdr1->extra, gz_hdr2->extra, gz_hdr1->extra_len);
		if (ret)
			return INCORRECT_EXTRA_BUF;
	}

	if (gz_hdr1->name != NULL && gz_hdr2->name != NULL) {
		max_len = gz_hdr1->name_buf_len;
		if (gz_hdr1->name_buf_len < gz_hdr2->name_buf_len)
			max_len = gz_hdr2->name_buf_len;

		ret = strncmp(gz_hdr1->name, gz_hdr2->name, max_len);
		if (ret)
			return INCORRECT_NAME;
	}

	if (gz_hdr1->comment != NULL && gz_hdr2->comment != NULL) {
		max_len = gz_hdr1->comment_buf_len;
		if (gz_hdr1->comment_buf_len < gz_hdr2->comment_buf_len)
			max_len = gz_hdr2->comment_buf_len;

		ret = strncmp(gz_hdr1->comment, gz_hdr2->comment, max_len);
		if (ret)
			return INCORRECT_COMMENT;
	}
	return ret;
}

int compare_zlib_headers(struct isal_zlib_header *z_hdr1, struct isal_zlib_header *z_hdr2)
{
	if (z_hdr1->info != z_hdr2->info)
		return INCORRECT_INFO;

	if (z_hdr1->level != z_hdr2->level)
		return INCORRECT_LEVEL;

	if (z_hdr1->dict_flag != z_hdr2->dict_flag)
		return INCORRECT_DICT_FLAG;

	if (z_hdr1->dict_flag && z_hdr1->dict_id != z_hdr2->dict_id)
		return INCORRECT_DICT_ID;

	return 0;
}

int read_gzip_header_simple(uint8_t * hdr_buf, uint32_t hdr_buf_len,
			    struct isal_gzip_header *gz_hdr_orig)
{

	int ret = 0;
	struct inflate_state state;
	struct isal_gzip_header gz_hdr;

	rand_buf((uint8_t *) & gz_hdr, sizeof(gz_hdr));
	gz_hdr.extra_buf_len = gz_hdr_orig->extra_buf_len;
	gz_hdr.name_buf_len = gz_hdr_orig->name_buf_len;
	gz_hdr.comment_buf_len = gz_hdr_orig->comment_buf_len;

	ret = malloc_gzip_header(&gz_hdr);
	if (ret) {
		print_gzip_final_verbose(hdr_buf, hdr_buf_len, gz_hdr_orig, NULL);
		print_error(ret);
		free_gzip_header(&gz_hdr);
		return ret;
	}

	isal_inflate_init(&state);
	state.next_in = hdr_buf;
	state.avail_in = hdr_buf_len;
	ret = isal_read_gzip_header(&state, &gz_hdr);

	if (ret) {
		print_gzip_final_verbose(hdr_buf, hdr_buf_len, gz_hdr_orig, &gz_hdr);
		print_error(ret);
		free_gzip_header(&gz_hdr);
		return ret;
	}

	ret = compare_gzip_headers(gz_hdr_orig, &gz_hdr);

	if (ret) {
		print_gzip_final_verbose(hdr_buf, hdr_buf_len, gz_hdr_orig, &gz_hdr);
		print_error(ret);
	}

	free_gzip_header(&gz_hdr);
	return ret;
}

int read_zlib_header_simple(uint8_t * hdr_buf, uint32_t hdr_buf_len,
			    struct isal_zlib_header *z_hdr_orig)
{

	int ret = 0;
	struct inflate_state state;
	struct isal_zlib_header z_hdr;

	rand_buf((uint8_t *) & z_hdr, sizeof(z_hdr));

	if (ret) {
		print_zlib_final_verbose(hdr_buf, hdr_buf_len, z_hdr_orig, NULL);
		print_error(ret);
		return ret;
	}

	isal_inflate_init(&state);
	state.next_in = hdr_buf;
	state.avail_in = hdr_buf_len;
	ret = isal_read_zlib_header(&state, &z_hdr);

	if (ret) {
		print_zlib_final_verbose(hdr_buf, hdr_buf_len, z_hdr_orig, &z_hdr);
		print_error(ret);
		return ret;
	}

	ret = compare_zlib_headers(z_hdr_orig, &z_hdr);

	if (ret) {
		print_zlib_final_verbose(hdr_buf, hdr_buf_len, z_hdr_orig, &z_hdr);
		print_error(ret);
	}

	return ret;
}

int read_gzip_header_streaming(uint8_t * hdr_buf, uint32_t hdr_buf_len,
			       struct isal_gzip_header *gz_hdr_orig)
{
	int ret = 0;
	uint32_t max_dec_size, dec_size, max_extra_len, extra_len;
	uint32_t max_name_len, name_len, max_comment_len, comment_len;
	struct inflate_state state;
	struct isal_gzip_header gz_hdr;
	void *tmp_ptr;

	rand_buf((uint8_t *) & gz_hdr, sizeof(gz_hdr));

	max_dec_size = (rand() % hdr_buf_len) + 2;

	max_extra_len = 2;
	max_name_len = 2;
	max_comment_len = 2;
	if (gz_hdr_orig->extra_buf_len)
		max_extra_len = (rand() % gz_hdr_orig->extra_buf_len) + 2;
	if (gz_hdr_orig->name_buf_len)
		max_name_len = (rand() % gz_hdr_orig->name_buf_len) + 2;
	if (gz_hdr_orig->comment_buf_len)
		max_comment_len = (rand() % gz_hdr_orig->comment_buf_len) + 2;

	extra_len = rand() % max_extra_len;
	name_len = rand() % max_name_len;
	comment_len = rand() % max_comment_len;

	if (extra_len == 0)
		extra_len = 1;
	if (name_len == 0)
		name_len = 1;
	if (comment_len == 0)
		comment_len = 1;

	gz_hdr.extra_buf_len = extra_len;
	gz_hdr.name_buf_len = name_len;
	gz_hdr.comment_buf_len = comment_len;

	ret = malloc_gzip_header(&gz_hdr);

	if (ret) {
		print_gzip_final_verbose(hdr_buf, hdr_buf_len, gz_hdr_orig, NULL);
		print_error(ret);
		free_gzip_header(&gz_hdr);
		return (ret == 0);
	}

	isal_inflate_init(&state);

	state.next_in = hdr_buf;
	dec_size = rand() % max_dec_size;
	if (dec_size > hdr_buf_len)
		dec_size = hdr_buf_len;

	state.avail_in = dec_size;
	hdr_buf_len -= dec_size;

	while (1) {
		ret = isal_read_gzip_header(&state, &gz_hdr);

		switch (ret) {
		case ISAL_NAME_OVERFLOW:
			if (name_len >= NAME_SIZE_MAX)
				break;

			name_len += rand() % max_name_len;
			tmp_ptr = realloc(gz_hdr.name, name_len);
			if (tmp_ptr == NULL) {
				ret = MALLOC_FAILED;
				break;
			}
			gz_hdr.name = tmp_ptr;
			gz_hdr.name_buf_len = name_len;
			continue;
		case ISAL_COMMENT_OVERFLOW:
			if (comment_len >= COMMENT_SIZE_MAX)
				break;

			comment_len += rand() % max_comment_len;
			tmp_ptr = realloc(gz_hdr.comment, comment_len);
			if (tmp_ptr == NULL) {
				ret = MALLOC_FAILED;
				break;
			}
			gz_hdr.comment = tmp_ptr;
			gz_hdr.comment_buf_len = comment_len;
			continue;
		case ISAL_EXTRA_OVERFLOW:
			if (extra_len >= EXTRA_SIZE_MAX)
				break;

			extra_len += rand() % max_extra_len;
			tmp_ptr = realloc(gz_hdr.extra, extra_len);
			if (tmp_ptr == NULL) {
				ret = MALLOC_FAILED;
				break;
			}
			gz_hdr.extra = tmp_ptr;
			gz_hdr.extra_buf_len = extra_len;
			continue;
		case ISAL_END_INPUT:
			if (hdr_buf_len == 0)
				break;

			dec_size = rand() % max_dec_size;
			if (dec_size > hdr_buf_len)
				dec_size = hdr_buf_len;

			state.avail_in = dec_size;
			hdr_buf_len -= dec_size;
			continue;
		}

		break;
	}

	if (ret) {
		print_gzip_final_verbose(hdr_buf, hdr_buf_len, gz_hdr_orig, &gz_hdr);
		print_error(ret);
		free_gzip_header(&gz_hdr);
		return ret;
	}

	ret = compare_gzip_headers(gz_hdr_orig, &gz_hdr);

	if (ret) {
		print_gzip_final_verbose(hdr_buf, hdr_buf_len, gz_hdr_orig, &gz_hdr);
		print_error(ret);
	}

	free_gzip_header(&gz_hdr);
	return ret;
}

int read_zlib_header_streaming(uint8_t * hdr_buf, uint32_t hdr_buf_len,
			       struct isal_zlib_header *z_hdr_orig)
{
	int ret = ISAL_END_INPUT;
	uint32_t max_dec_size, dec_size;
	struct inflate_state state;
	struct isal_zlib_header z_hdr;

	rand_buf((uint8_t *) & z_hdr, sizeof(z_hdr));

	max_dec_size = (rand() % hdr_buf_len) + 2;

	isal_inflate_init(&state);

	state.next_in = hdr_buf;
	while (ret == ISAL_END_INPUT && hdr_buf_len > 0) {
		dec_size = rand() % max_dec_size;
		if (dec_size > hdr_buf_len)
			dec_size = hdr_buf_len;

		state.avail_in = dec_size;
		hdr_buf_len -= dec_size;

		ret = isal_read_zlib_header(&state, &z_hdr);
	}

	if (ret) {
		print_zlib_final_verbose(hdr_buf, hdr_buf_len, z_hdr_orig, &z_hdr);
		print_error(ret);
		return ret;
	}

	ret = compare_zlib_headers(z_hdr_orig, &z_hdr);

	if (ret) {
		print_zlib_final_verbose(hdr_buf, hdr_buf_len, z_hdr_orig, &z_hdr);
		print_error(ret);
	}

	return ret;
}

int main(int argc, char *argv[])
{
	uint8_t *hdr_buf;
	uint32_t hdr_buf_len;
	int ret = 0, fin_ret = 0;
	struct isal_gzip_header gz_hdr_orig;
	struct isal_zlib_header z_hdr_orig;
	int i;

#ifndef VERBOSE
	setbuf(stdout, NULL);
#endif
	printf("Test Seed  : %d\n", TEST_SEED);
	printf("Randoms    : %d\n", RANDOMS);
	srand(TEST_SEED);

	printf("gzip wrapper test: ");
	for (i = 0; i < RANDOMS; i++) {
		rand_buf((uint8_t *) & gz_hdr_orig, sizeof(gz_hdr_orig));

		ret = gen_rand_gzip_header(&gz_hdr_orig);
		if (ret) {
			print_error(ret);
			return (ret == 0);
		}

		hdr_buf_len = gzip_header_size(&gz_hdr_orig);
		hdr_buf = malloc(hdr_buf_len);

		ret = write_gzip_header(hdr_buf, hdr_buf_len, &gz_hdr_orig);

		fin_ret |= ret;
		if (ret)
			return (ret == 0);

		ret = read_gzip_header_simple(hdr_buf, hdr_buf_len, &gz_hdr_orig);

		fin_ret |= ret;
		if (ret)
			return (ret == 0);

		ret = read_gzip_header_streaming(hdr_buf, hdr_buf_len, &gz_hdr_orig);

		fin_ret |= ret;
		if (ret)
			return (ret == 0);

		free_gzip_header(&gz_hdr_orig);
		if (hdr_buf != NULL)
			free(hdr_buf);

		if (i % (RANDOMS / 16) == 0)
			printf(".");
	}
	printf("Pass \n");

	printf("zlib wrapper test: ");
	for (i = 0; i < RANDOMS; i++) {
		memset(&z_hdr_orig, 0, sizeof(z_hdr_orig));

		gen_rand_zlib_header(&z_hdr_orig);

		hdr_buf_len = zlib_header_size(&z_hdr_orig);
		hdr_buf = malloc(hdr_buf_len);

		ret = write_zlib_header(hdr_buf, hdr_buf_len, &z_hdr_orig);

		fin_ret |= ret;
		if (ret)
			return (ret == 0);

		ret = read_zlib_header_simple(hdr_buf, hdr_buf_len, &z_hdr_orig);

		fin_ret |= ret;
		if (ret)
			return (ret == 0);

		ret = read_zlib_header_streaming(hdr_buf, hdr_buf_len, &z_hdr_orig);

		fin_ret |= ret;
		if (ret)
			return (ret == 0);

		if (hdr_buf != NULL)
			free(hdr_buf);

		if (i % (RANDOMS / 16) == 0)
			printf(".");
	}
	printf("Pass \n");

	printf("igzip wrapper_hdr test finished:%s \n",
	       fin_ret ? " Some tests failed " : " All tests passed");

	return 0;
}
