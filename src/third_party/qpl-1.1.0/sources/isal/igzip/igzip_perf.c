/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <getopt.h>
#include "huff_codes.h"
#include "igzip_lib.h"
#include "test.h"

#include <zlib.h>

#define BUF_SIZE 1024

#define OPTARGS "hl:f:z:i:d:stub:y:w:o:"

#define COMPRESSION_QUEUE_LIMIT 32
#define UNSET -1

#define xstr(a) str(a)
#define str(a) #a

/* Limit output buffer size to 2 Gigabytes. Since stream->avail_out is a
 * uint32_t and there is no logic for handling an overflowed output buffer in
 * the perf test, this define must be less then 4 Gigabytes */
#define MAX_COMPRESS_BUF_SIZE (1U << 31)

int level_size_buf[10] = {
#ifdef ISAL_DEF_LVL0_DEFAULT
	ISAL_DEF_LVL0_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL1_DEFAULT
	ISAL_DEF_LVL1_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL2_DEFAULT
	ISAL_DEF_LVL2_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL3_DEFAULT
	ISAL_DEF_LVL3_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL4_DEFAULT
	ISAL_DEF_LVL4_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL5_DEFAULT
	ISAL_DEF_LVL5_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL6_DEFAULT
	ISAL_DEF_LVL6_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL7_DEFAULT
	ISAL_DEF_LVL7_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL8_DEFAULT
	ISAL_DEF_LVL8_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL9_DEFAULT
	ISAL_DEF_LVL9_DEFAULT,
#else
	0,
#endif
};

enum {
	ISAL_STATELESS,
	ISAL_STATEFUL,
	ZLIB
};

struct compress_strategy {
	int32_t mode;
	int32_t level;
};

struct inflate_modes {
	int32_t stateless;
	int32_t stateful;
	int32_t zlib;
};

struct perf_info {
	char *file_name;
	size_t file_size;
	size_t deflate_size;
	uint32_t inblock_size;
	uint32_t flush_type;
	int32_t hist_bits;
	int32_t deflate_time;
	int32_t inflate_time;
	struct compress_strategy strategy;
	uint32_t inflate_mode;
	struct perf start;
};

void init_perf_info(struct perf_info *info)
{
	memset(info, 0, sizeof(*info));
	info->deflate_time = BENCHMARK_TIME;
	info->inflate_time = BENCHMARK_TIME;
}

int usage(void)
{
	fprintf(stderr,
		"Usage: igzip_perf [options] <infile>\n"
		"  -h          help, print this message\n"
		"  The options -l, -f, -z may be used up to "
		xstr(COMPRESSION_QUEUE_LIMIT) " times\n"
		"  -l <level>  isa-l stateless deflate level to test ("
		xstr(ISAL_DEF_MIN_LEVEL) "-" xstr(ISAL_DEF_MAX_LEVEL) ")\n"
		"  -f <level>  isa-l stateful deflate level to test ("
		xstr(ISAL_DEF_MIN_LEVEL) "-" xstr(ISAL_DEF_MAX_LEVEL) ")\n"
		"  -z <level>  zlib  deflate level to test\n"
		"  -d <time>   approx time in seconds for deflate (at least 0)\n"
		"  -i <time>   approx time in seconds for inflate (at least 0)\n"
		"  -s          performance test isa-l stateful inflate\n"
		"  -t          performance test isa-l stateless inflate\n"
		"  -u          performance test zlib inflate\n"
		"  -o <file>   output file to store compressed data (last one if multiple)\n"
		"  -b <size>   input buffer size, applies to stateful options (-f,-z,-s)\n"
		"  -y <type>   flush type: 0 (default: no flush), 1 (sync flush), 2 (full flush)\n"
		"  -w <size>   log base 2 size of history window, between 9 and 15\n");
	exit(0);
}

void print_perf_info_line(struct perf_info *info)
{
	printf("igzip_perf-> compress level: %d flush_type: %d block_size: %d\n",
	       info->strategy.level, info->flush_type, info->inblock_size);
}

void print_file_line(struct perf_info *info)
{
	printf("  file info-> name: %s file_size: %lu compress_size: %lu ratio: %2.02f%%\n",
	       info->file_name, info->file_size, info->deflate_size,
	       100.0 * info->deflate_size / info->file_size);
}

void print_deflate_perf_line(struct perf_info *info)
{
	if (info->strategy.mode == ISAL_STATELESS)
		printf("    isal_stateless_deflate-> ");
	else if (info->strategy.mode == ISAL_STATEFUL)
		printf("    isal_stateful_deflate->  ");
	else if (info->strategy.mode == ZLIB)
		printf("    zlib_deflate->           ");

	perf_print(info->start, info->file_size);
}

void print_inflate_perf_line(struct perf_info *info)
{
	if (info->inflate_mode == ISAL_STATELESS)
		printf("    isal_stateless_inflate-> ");
	else if (info->inflate_mode == ISAL_STATEFUL)
		printf("    isal_stateful_inflate->  ");
	else if (info->inflate_mode == ZLIB)
		printf("    zlib_inflate->           ");

	perf_print(info->start, info->file_size);
}

int isal_deflate_round(struct isal_zstream *stream, uint8_t * outbuf, uint32_t outbuf_size,
		       uint8_t * inbuf, uint32_t inbuf_size,
		       uint32_t level, uint8_t * level_buf, uint32_t level_buf_size,
		       int flush_type, int hist_bits)
{
	int check;

	/* Setup stream for stateless compression */
	isal_deflate_init(stream);
	stream->end_of_stream = 1;	/* Do the entire file at once */
	stream->flush = flush_type;
	stream->next_in = inbuf;
	stream->avail_in = inbuf_size;
	stream->next_out = outbuf;
	stream->avail_out = outbuf_size;
	stream->level = level;
	stream->level_buf = level_buf;
	stream->level_buf_size = level_buf_size;
	stream->hist_bits = hist_bits;

	/* Compress stream */
	check = isal_deflate_stateless(stream);

	/* Verify compression success */
	if (check || stream->avail_in)
		return 1;

	return 0;
}

int isal_inflate_round(struct inflate_state *state, uint8_t * inbuf, uint32_t inbuf_size,
		       uint8_t * outbuf, uint32_t outbuf_size, int hist_bits)
{
	int check = 0;

	/* Setup for stateless inflate */
	state->next_in = inbuf;
	state->avail_in = inbuf_size;
	state->next_out = outbuf;
	state->avail_out = outbuf_size;
	state->crc_flag = ISAL_DEFLATE;
	state->hist_bits = hist_bits;

	/* Inflate data */
	check = isal_inflate_stateless(state);

	/* Verify inflate was successful */
	if (check)
		return 1;

	return 0;
}

int isal_deflate_stateful_round(struct isal_zstream *stream, uint8_t * outbuf,
				uint32_t outbuf_size, uint8_t * inbuf,
				uint32_t inbuf_size, uint32_t in_block_size, uint32_t level,
				uint8_t * level_buf, uint32_t level_buf_size, int flush_type,
				int hist_bits)
{
	uint64_t inbuf_remaining;
	int check = COMP_OK;

	/* Setup stream for stateful compression */
	inbuf_remaining = inbuf_size;
	isal_deflate_init(stream);
	stream->flush = flush_type;
	stream->next_in = inbuf;
	stream->next_out = outbuf;
	stream->avail_out = outbuf_size;
	stream->level = level;
	stream->level_buf = level_buf;
	stream->level_buf_size = level_buf_size;
	stream->hist_bits = hist_bits;

	/* Keep compressing so long as more data is available and no error has
	 * been hit */
	while (COMP_OK == check && inbuf_remaining > in_block_size) {
		/* Setup next in buffer, assumes out buffer is sufficiently
		 * large */
		stream->avail_in = in_block_size;
		inbuf_remaining -= in_block_size;

		/* Compress stream */
		check = isal_deflate(stream);
	}

	/* Finish compressing all remaining input */
	if (COMP_OK == check) {
		stream->avail_in = inbuf_remaining;
		stream->end_of_stream = 1;
		check = isal_deflate(stream);
	}

	/* Verify Compression Success */
	if (COMP_OK != check || stream->avail_in > 0)
		return 1;

	return 0;
}

int isal_inflate_stateful_round(struct inflate_state *state, uint8_t * inbuf,
				uint32_t inbuf_size, uint32_t in_block_size, uint8_t * outbuf,
				uint32_t outbuf_size, int hist_bits)
{
	int check = ISAL_DECOMP_OK;
	uint64_t inbuf_remaining;

	isal_inflate_init(state);
	state->next_in = inbuf;
	state->next_out = outbuf;
	state->avail_out = outbuf_size;
	state->hist_bits = hist_bits;
	inbuf_remaining = inbuf_size;

	while (ISAL_DECOMP_OK == check && inbuf_remaining >= in_block_size) {
		state->avail_in = in_block_size;
		inbuf_remaining -= in_block_size;
		check = isal_inflate(state);
	}
	if (ISAL_DECOMP_OK == check && inbuf_remaining > 0) {
		state->avail_in = inbuf_remaining;
		check = isal_inflate(state);
	}

	if (ISAL_DECOMP_OK != check || state->avail_in > 0)
		return 1;

	return 0;
}

int zlib_deflate_round(z_stream * gstream, uint8_t * outbuf, uInt outbuf_size,
		       uint8_t * inbuf, uLong inbuf_size,
		       uLong in_block_size, int level, int flush_type)
{
	uLong inbuf_remaining;
	int check = Z_OK;

	inbuf_remaining = inbuf_size;

	/* Setup stream for stateful compression */
	if (0 != deflateReset(gstream))
		return 1;

	gstream->next_in = inbuf;
	gstream->next_out = outbuf;
	gstream->avail_out = outbuf_size;

	/* Keep compressing so long as more data is available and no error has
	 * been hit */
	while (Z_OK == check && inbuf_remaining > in_block_size) {
		gstream->avail_in = in_block_size;
		inbuf_remaining -= in_block_size;
		check = deflate(gstream, flush_type);
	}

	/* Finish compressing all remaining input */
	if (Z_OK == check) {
		gstream->avail_in = inbuf_remaining;
		check = deflate(gstream, Z_FINISH);
	}

	/* Verify Compression Success */
	if (Z_STREAM_END != check)
		return 1;

	return 0;
}

int zlib_inflate_round(z_stream * gstream, uint8_t * inbuf,
		       uLong inbuf_size, uint8_t * outbuf, uInt outbuf_size)
{
	int check = 0;

	if (0 != inflateReset(gstream))
		return 1;

	gstream->next_in = inbuf;
	gstream->avail_in = inbuf_size;
	gstream->next_out = outbuf;
	gstream->avail_out = outbuf_size;
	check = inflate(gstream, Z_FINISH);
	if (check != Z_STREAM_END)
		return 1;

	return 0;
}

int isal_deflate_perf(uint8_t * outbuf, uint64_t * outbuf_size, uint8_t * inbuf,
		      uint64_t inbuf_size, int level, int flush_type, int hist_bits, int time,
		      struct perf *start)
{
	struct isal_zstream stream;
	uint8_t *level_buf = NULL;
	int check;

	if (level_size_buf[level] > 0) {
		level_buf = malloc(level_size_buf[level]);
		if (level_buf == NULL)
			return 1;
	}

	BENCHMARK(start, time, check =
		  isal_deflate_round(&stream, outbuf, *outbuf_size, inbuf,
				     inbuf_size, level, level_buf,
				     level_size_buf[level], flush_type, hist_bits));
	*outbuf_size = stream.total_out;
	return check;
}

int isal_deflate_stateful_perf(uint8_t * outbuf, uint64_t * outbuf_size, uint8_t * inbuf,
			       uint64_t inbuf_size, int level, int flush_type,
			       uint64_t in_block_size, int hist_bits, int time,
			       struct perf *start)
{
	struct isal_zstream stream;
	uint8_t *level_buf = NULL;
	int check;

	if (in_block_size == 0)
		in_block_size = inbuf_size;

	if (level_size_buf[level] > 0) {
		level_buf = malloc(level_size_buf[level]);
		if (level_buf == NULL)
			return 1;
	}

	BENCHMARK(start, time, check =
		  isal_deflate_stateful_round(&stream, outbuf, *outbuf_size, inbuf, inbuf_size,
					      in_block_size, level, level_buf,
					      level_size_buf[level], flush_type, hist_bits));
	*outbuf_size = stream.total_out;
	return check;

}

int zlib_deflate_perf(uint8_t * outbuf, uint64_t * outbuf_size, uint8_t * inbuf,
		      uint64_t inbuf_size, int level, int flush_type,
		      uint64_t in_block_size, int hist_bits, int time, struct perf *start)
{
	int check;
	z_stream gstream;
	int flush_translator[] = { Z_NO_FLUSH, Z_SYNC_FLUSH, Z_FULL_FLUSH };

	if (in_block_size == 0)
		in_block_size = inbuf_size;

	flush_type = flush_translator[flush_type];

	/* Initialize the gstream buffer */
	gstream.next_in = inbuf;
	gstream.avail_in = inbuf_size;
	gstream.zalloc = Z_NULL;
	gstream.zfree = Z_NULL;
	gstream.opaque = Z_NULL;

	if (hist_bits == 0)
		hist_bits = -15;
	else
		hist_bits = -hist_bits;

	if (0 != deflateInit2(&gstream, level, Z_DEFLATED, hist_bits, 9, Z_DEFAULT_STRATEGY))
		return 1;

	BENCHMARK(start, time, check =
		  zlib_deflate_round(&gstream, outbuf, *outbuf_size, inbuf, inbuf_size,
				     in_block_size, level, flush_type));

	*outbuf_size = gstream.total_out;
	deflateEnd(&gstream);

	return check;
}

int isal_inflate_perf(uint8_t * inbuf, uint64_t inbuf_size, uint8_t * outbuf,
		      uint64_t outbuf_size, uint8_t * filebuf, uint64_t file_size,
		      int hist_bits, int time, struct perf *start)
{
	struct inflate_state state;
	int check;

	/* Check that data decompresses */
	check = isal_inflate_round(&state, inbuf, inbuf_size, outbuf, outbuf_size, hist_bits);
	if (check || state.total_out != file_size || memcmp(outbuf, filebuf, file_size))
		return 1;

	BENCHMARK(start, time, isal_inflate_round(&state, inbuf, inbuf_size,
						  outbuf, outbuf_size, hist_bits));

	return check;
}

int isal_inflate_stateful_perf(uint8_t * inbuf, uint64_t inbuf_size, uint8_t * outbuf,
			       uint64_t outbuf_size, uint8_t * filebuf, uint64_t file_size,
			       uint64_t in_block_size, int hist_bits, int time,
			       struct perf *start)
{
	struct inflate_state state;
	int check;

	if (in_block_size == 0)
		in_block_size = inbuf_size;

	check = isal_inflate_round(&state, inbuf, inbuf_size, outbuf, outbuf_size, hist_bits);
	if (check || state.total_out != file_size || memcmp(outbuf, filebuf, file_size))
		return 1;

	BENCHMARK(start, time,
		  isal_inflate_stateful_round(&state, inbuf, inbuf_size, in_block_size, outbuf,
					      outbuf_size, hist_bits));

	return 0;

}

int zlib_inflate_perf(uint8_t * inbuf, uint64_t inbuf_size, uint8_t * outbuf,
		      uint64_t outbuf_size, uint8_t * filebuf, uint64_t file_size,
		      int hist_bits, int time, struct perf *start)
{
	int check;
	z_stream gstream;

	gstream.next_in = inbuf;
	gstream.avail_in = inbuf_size;
	gstream.zalloc = Z_NULL;
	gstream.zfree = Z_NULL;
	gstream.opaque = Z_NULL;

	if (hist_bits == 0)
		hist_bits = -15;
	else
		hist_bits = -hist_bits;

	if (0 != inflateInit2(&gstream, hist_bits))
		return 1;

	check = zlib_inflate_round(&gstream, inbuf, inbuf_size, outbuf, outbuf_size);
	if (check || gstream.total_out != file_size || memcmp(outbuf, filebuf, file_size))
		return 1;

	BENCHMARK(start, time,
		  zlib_inflate_round(&gstream, inbuf, inbuf_size, outbuf, outbuf_size));

	inflateEnd(&gstream);
	return 0;
}

int main(int argc, char *argv[])
{
	FILE *in = NULL;
	unsigned char *compressbuf, *decompbuf, *filebuf;
	char *outfile = NULL;
	int i, c, ret = 0;
	uint64_t decompbuf_size, compressbuf_size;
	uint64_t block_count;

	struct compress_strategy compression_queue[COMPRESSION_QUEUE_LIMIT];

	int compression_queue_size = 0;
	struct compress_strategy compress_strat;
	struct inflate_modes inflate_strat = { 0 };
	struct perf_info info;
	init_perf_info(&info);

	while ((c = getopt(argc, argv, OPTARGS)) != -1) {
		switch (c) {
		case 'l':
			if (compression_queue_size >= COMPRESSION_QUEUE_LIMIT) {
				printf("Too many levels specified");
				exit(0);
			}

			compress_strat.mode = ISAL_STATELESS;
			compress_strat.level = atoi(optarg);
			if (compress_strat.level > ISAL_DEF_MAX_LEVEL) {
				printf("Unsupported isa-l compression level\n");
				exit(0);
			}

			compression_queue[compression_queue_size] = compress_strat;
			compression_queue_size++;
			break;
		case 'f':
			if (compression_queue_size >= COMPRESSION_QUEUE_LIMIT) {
				printf("Too many levels specified");
				exit(0);
			}

			compress_strat.mode = ISAL_STATEFUL;
			compress_strat.level = atoi(optarg);
			if (compress_strat.level > ISAL_DEF_MAX_LEVEL) {
				printf("Unsupported isa-l compression level\n");
				exit(0);
			}

			compression_queue[compression_queue_size] = compress_strat;
			compression_queue_size++;
			break;
		case 'z':
			if (compression_queue_size >= COMPRESSION_QUEUE_LIMIT) {
				printf("Too many levels specified");
				exit(0);
			}

			compress_strat.mode = ZLIB;
			compress_strat.level = atoi(optarg);
			if (compress_strat.level > Z_BEST_COMPRESSION) {
				printf("Unsupported zlib compression level\n");
				exit(0);
			}
			compression_queue[compression_queue_size] = compress_strat;
			compression_queue_size++;
			break;
		case 'i':
			info.inflate_time = atoi(optarg);
			if (info.inflate_time < 0)
				usage();
			break;
		case 'd':
			info.deflate_time = atoi(optarg);
			if (info.deflate_time < 0)
				usage();
			break;
		case 's':
			inflate_strat.stateful = 1;
			break;
		case 't':
			inflate_strat.stateless = 1;
			break;
		case 'u':
			inflate_strat.zlib = 1;
			break;
		case 'b':
			inflate_strat.stateful = 1;
			info.inblock_size = atoi(optarg);
			break;
		case 'y':
			info.flush_type = atoi(optarg);
			if (info.flush_type != NO_FLUSH && info.flush_type != SYNC_FLUSH
			    && info.flush_type != FULL_FLUSH) {
				printf("Unsupported flush type\n");
				exit(0);
			}
			break;

		case 'w':
			info.hist_bits = atoi(optarg);
			if (info.hist_bits > 15 || info.hist_bits < 9)
				usage();
			break;
		case 'o':
			outfile = optarg;
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}

	if (optind >= argc)
		usage();

	if (!inflate_strat.stateless && !inflate_strat.stateful && !inflate_strat.zlib) {
		if (info.inblock_size == 0)
			inflate_strat.stateless = 1;
		else
			inflate_strat.stateful = 1;
	}

	/* Allocate space for entire input file and output
	 * (assuming some possible expansion on output size)
	 */
	info.file_name = argv[optind];
	in = fopen(info.file_name, "rb");
	if (NULL == in) {
		printf("Error: Can not find file %s\n", info.file_name);
		exit(0);
	}

	info.file_size = get_filesize(in);
	if (info.file_size == 0) {
		printf("Error: input file has 0 size\n");
		exit(0);
	}

	decompbuf_size = info.file_size;

	if (compression_queue_size == 0) {
		if (info.inblock_size == 0)
			compression_queue[0].mode = ISAL_STATELESS;
		else
			compression_queue[0].mode = ISAL_STATEFUL;
		compression_queue[0].level = 1;
		compression_queue_size = 1;
	}

	filebuf = malloc(info.file_size);
	if (filebuf == NULL) {
		fprintf(stderr, "Can't allocate temp buffer memory\n");
		exit(0);
	}

	block_count = 1;
	if (info.flush_type > 0)
		block_count = (info.file_size + info.inblock_size - 1) / info.inblock_size;

	/* Way overestimate likely compressed size to handle bad type 0 and
	 * small block_size case */
	compressbuf_size = block_count * ISAL_DEF_MAX_HDR_SIZE + 2 * info.file_size;
	if (compressbuf_size >= MAX_COMPRESS_BUF_SIZE)
		compressbuf_size = MAX_COMPRESS_BUF_SIZE;

	compressbuf = malloc(compressbuf_size);
	if (compressbuf == NULL) {
		fprintf(stderr, "Can't allocate input buffer memory\n");
		exit(0);
	}

	decompbuf = malloc(decompbuf_size);
	if (decompbuf == NULL) {
		fprintf(stderr, "Can't allocate output buffer memory\n");
		exit(0);
	}

	if (info.file_size != fread(filebuf, 1, info.file_size, in)) {
		fprintf(stderr, "Could not read in all input\n");
		exit(0);
	}
	fclose(in);

	for (i = 0; i < compression_queue_size; i++) {
		if (i > 0)
			printf("\n\n");

		info.strategy = compression_queue[i];
		print_perf_info_line(&info);

		info.deflate_size = compressbuf_size;

		if (info.strategy.mode == ISAL_STATELESS)
			ret = isal_deflate_perf(compressbuf, &info.deflate_size, filebuf,
						info.file_size, compression_queue[i].level,
						info.flush_type, info.hist_bits,
						info.deflate_time, &info.start);
		else if (info.strategy.mode == ISAL_STATEFUL)
			ret =
			    isal_deflate_stateful_perf(compressbuf, &info.deflate_size,
						       filebuf, info.file_size,
						       compression_queue[i].level,
						       info.flush_type, info.inblock_size,
						       info.hist_bits, info.deflate_time,
						       &info.start);
		else if (info.strategy.mode == ZLIB)
			ret = zlib_deflate_perf(compressbuf, &info.deflate_size, filebuf,
						info.file_size, compression_queue[i].level,
						info.flush_type, info.inblock_size,
						info.hist_bits, info.deflate_time,
						&info.start);

		if (ret) {
			printf("  Error in compression\n");
			continue;
		}

		print_file_line(&info);
		printf("\n");
		print_deflate_perf_line(&info);
		printf("\n");

		if (outfile != NULL && i + 1 == compression_queue_size) {
			FILE *out;
			out = fopen(outfile, "wb");
			fwrite(compressbuf, 1, info.deflate_size, out);
			fclose(out);
		}

		if (info.inflate_time == 0)
			continue;

		if (inflate_strat.stateless) {
			info.inflate_mode = ISAL_STATELESS;
			ret = isal_inflate_perf(compressbuf, info.deflate_size, decompbuf,
						decompbuf_size, filebuf, info.file_size,
						info.hist_bits, info.inflate_time,
						&info.start);
			if (ret)
				printf("    Error in isal stateless inflate\n");
			else
				print_inflate_perf_line(&info);
		}

		if (inflate_strat.stateful) {
			info.inflate_mode = ISAL_STATEFUL;
			ret =
			    isal_inflate_stateful_perf(compressbuf, info.deflate_size,
						       decompbuf, decompbuf_size, filebuf,
						       info.file_size, info.inblock_size,
						       info.hist_bits, info.inflate_time,
						       &info.start);

			if (ret)
				printf("    Error in isal stateful inflate\n");
			else
				print_inflate_perf_line(&info);
		}

		if (inflate_strat.zlib) {
			info.inflate_mode = ZLIB;
			ret = zlib_inflate_perf(compressbuf, info.deflate_size, decompbuf,
						decompbuf_size, filebuf, info.file_size,
						info.hist_bits, info.inflate_time,
						&info.start);
			if (ret)
				printf("    Error in zlib inflate\n");
			else
				print_inflate_perf_line(&info);
		}
	}

	free(compressbuf);
	free(decompbuf);
	free(filebuf);
	return 0;
}
