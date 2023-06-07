/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/* This program can be used to generate custom a custom huffman encoding to get
 * better data compression. This is most useful when the type of data being
 * compressed is well known.
 *
 * To use generate_custom_hufftables, pass a sequence of files to the program
 * that together form an accurate representation of the data that is being
 * compressed. Generate_custom_hufftables will then produce the file
 * hufftables_c.c, which should be moved to replace its counterpart in the igzip
 * source folder. After recompiling the Isa-l library, the igzip compression
 * functions will use the new hufftables.
 *
 * Generate_custom_hufftables should be compiled with the same compile time
 * parameters as the igzip source code. Generating custom hufftables with
 * different compile time parameters may cause igzip to produce invalid output
 * for the reasons described below. The default parameters used by
 * generate_custom_hufftables are the same as the default parameters used by
 * igzip.
 *
 * *WARNING* generate custom hufftables must be compiled with a IGZIP_HIST_SIZE
 * that is at least as large as the IGZIP_HIST_SIZE used by igzip. By default
 * IGZIP_HIST_SIZE is 32K, the maximum usable IGZIP_HIST_SIZE is 32K. The reason
 * for this is to generate better compression. Igzip cannot produce look back
 * distances with sizes larger than the IGZIP_HIST_SIZE igzip was compiled with,
 * so look back distances with sizes larger than IGZIP_HIST_SIZE are not
 * assigned a huffman code. The definition of LONGER_HUFFTABLES must be
 * consistent as well since that definition changes the size of the structures
 * printed by this tool.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include "igzip_lib.h"

#include "huff_codes.h"
#include "huffman.h"

/*These max code lengths are limited by how the data is stored in
 * hufftables.asm. The deflate standard max is 15.*/

#define MAX_HEADER_SIZE ISAL_DEF_MAX_HDR_SIZE

#define GZIP_HEADER_SIZE 10
#define GZIP_TRAILER_SIZE 8
#define ZLIB_HEADER_SIZE 2
#define ZLIB_TRAILER_SIZE 4

/**
 * @brief Prints a table of uint8_t elements to a file.
 * @param outfile: the file the table is printed to.
 * @param table: the table to be printed.
 * @param length: number of elements to be printed.
 * @param header: header to append in front of the table.
 * @param footer: footer to append at the end of the table.
 * @param begin_line: string printed at beginning of new line
 */
void fprint_uint8_table(FILE * outfile, uint8_t * table, uint64_t length, char *header,
			char *footer, char *begin_line)
{
	int i;
	fprintf(outfile, "%s", header);
	for (i = 0; i < length - 1; i++) {
		if ((i & 7) == 0)
			fprintf(outfile, "\n%s", begin_line);
		else
			fprintf(outfile, " ");
		fprintf(outfile, "0x%02x,", table[i]);
	}

	if ((i & 7) == 0)
		fprintf(outfile, "\n%s", begin_line);
	else
		fprintf(outfile, " ");
	fprintf(outfile, "0x%02x", table[i]);
	fprintf(outfile, "%s", footer);

}

/**
 * @brief Prints a table of uint16_t elements to a file.
 * @param outfile: the file the table is printed to.
 * @param table: the table to be printed.
 * @param length: number of elements to be printed.
 * @param header: header to append in front of the table.
 * @param footer: footer to append at the end of the table.
 * @param begin_line: string printed at beginning of new line
 */
void fprint_uint16_table(FILE * outfile, uint16_t * table, uint64_t length, char *header,
			 char *footer, char *begin_line)
{
	int i;
	fprintf(outfile, "%s", header);
	for (i = 0; i < length - 1; i++) {
		if ((i & 7) == 0)
			fprintf(outfile, "\n%s", begin_line);
		else
			fprintf(outfile, " ");
		fprintf(outfile, "0x%04x,", table[i]);
	}

	if ((i & 7) == 0)
		fprintf(outfile, "\n%s", begin_line);
	else
		fprintf(outfile, " ");
	fprintf(outfile, "0x%04x", table[i]);
	fprintf(outfile, "%s", footer);

}

/**
 * @brief Prints a table of uint32_t elements to a file.
 * @param outfile: the file the table is printed to.
 * @param table: the table to be printed.
 * @param length: number of elements to be printed.
 * @param header: header to append in front of the table.
 * @param footer: footer to append at the end of the table.
 * @param begin_line: string printed at beginning of new line
 */
void fprint_uint32_table(FILE * outfile, uint32_t * table, uint64_t length, char *header,
			 char *footer, char *begin_line)
{
	int i;
	fprintf(outfile, "%s", header);
	for (i = 0; i < length - 1; i++) {
		if ((i & 3) == 0)
			fprintf(outfile, "\n%s", begin_line);
		else
			fprintf(outfile, " ");
		fprintf(outfile, "0x%08x,", table[i]);
	}

	if ((i & 3) == 0)
		fprintf(outfile, "%s", begin_line);
	else
		fprintf(outfile, " ");
	fprintf(outfile, "0x%08x", table[i]);
	fprintf(outfile, "%s", footer);

}

void fprint_hufftables(FILE * output_file, char *hufftables_name,
		       struct isal_hufftables *hufftables)
{
	fprintf(output_file, "struct isal_hufftables %s = {\n\n", hufftables_name);

	fprint_uint8_table(output_file, hufftables->deflate_hdr,
			   hufftables->deflate_hdr_count +
			   (hufftables->deflate_hdr_extra_bits + 7) / 8,
			   "\t.deflate_hdr = {", "},\n\n", "\t\t");

	fprintf(output_file, "\t.deflate_hdr_count = %d,\n", hufftables->deflate_hdr_count);
	fprintf(output_file, "\t.deflate_hdr_extra_bits = %d,\n\n",
		hufftables->deflate_hdr_extra_bits);

	fprint_uint32_table(output_file, hufftables->dist_table, IGZIP_DIST_TABLE_SIZE,
			    "\t.dist_table = {", "},\n\n", "\t\t");

	fprint_uint32_table(output_file, hufftables->len_table, IGZIP_LEN_TABLE_SIZE,
			    "\t.len_table = {", "},\n\n", "\t\t");

	fprint_uint16_table(output_file, hufftables->lit_table, IGZIP_LIT_TABLE_SIZE,
			    "\t.lit_table = {", "},\n\n", "\t\t");
	fprint_uint8_table(output_file, hufftables->lit_table_sizes, IGZIP_LIT_TABLE_SIZE,
			   "\t.lit_table_sizes = {", "},\n\n", "\t\t");

	fprint_uint16_table(output_file, hufftables->dcodes,
			    ISAL_DEF_DIST_SYMBOLS - IGZIP_DECODE_OFFSET,
			    "\t.dcodes = {", "},\n\n", "\t\t");
	fprint_uint8_table(output_file, hufftables->dcodes_sizes,
			   ISAL_DEF_DIST_SYMBOLS - IGZIP_DECODE_OFFSET,
			   "\t.dcodes_sizes = {", "}\n", "\t\t");
	fprintf(output_file, "};\n");
}

void fprint_header(FILE * output_file)
{

	fprintf(output_file, "#include <stdint.h>\n");
	fprintf(output_file, "#include <igzip_lib.h>\n\n");

	fprintf(output_file, "#if IGZIP_HIST_SIZE > %d\n"
		"# error \"Invalid history size for the custom hufftable\"\n"
		"#endif\n", IGZIP_HIST_SIZE);

#ifdef LONGER_HUFFTABLE
	fprintf(output_file, "#ifndef LONGER_HUFFTABLE\n"
		"# error \"Custom hufftable requires LONGER_HUFFTABLE to be defined \"\n"
		"#endif\n");
#else
	fprintf(output_file, "#ifdef LONGER_HUFFTABLE\n"
		"# error \"Custom hufftable requires LONGER_HUFFTABLE to not be defined \"\n"
		"#endif\n");
#endif
	fprintf(output_file, "\n");

	fprintf(output_file, "const uint8_t gzip_hdr[] = {\n"
		"\t0x1f, 0x8b, 0x08, 0x00, 0x00,\n" "\t0x00, 0x00, 0x00, 0x00, 0xff\t};\n\n");

	fprintf(output_file, "const uint32_t gzip_hdr_bytes = %d;\n", GZIP_HEADER_SIZE);
	fprintf(output_file, "const uint32_t gzip_trl_bytes = %d;\n\n", GZIP_TRAILER_SIZE);

	fprintf(output_file, "const uint8_t zlib_hdr[] = { 0x78, 0x01 };\n\n");
	fprintf(output_file, "const uint32_t zlib_hdr_bytes = %d;\n", ZLIB_HEADER_SIZE);
	fprintf(output_file, "const uint32_t zlib_trl_bytes = %d;\n", ZLIB_TRAILER_SIZE);
}

static uint32_t convert_dist_to_dist_sym(uint32_t dist)
{
	assert(dist <= 32768 && dist > 0);
	if (dist <= 32768) {
		uint32_t msb = dist > 4 ? bsr(dist - 1) - 2 : 0;
		return (msb * 2) + ((dist - 1) >> msb);
	} else {
		return ~0;
	}
}

/**
 * @brief  Returns the deflate symbol value for a repeat length.
 */
static uint32_t convert_length_to_len_sym(uint32_t length)
{
	assert(length > 2 && length < 259);

	/* Based on tables on page 11 in RFC 1951 */
	if (length < 11)
		return 257 + length - 3;
	else if (length < 19)
		return 261 + (length - 3) / 2;
	else if (length < 35)
		return 265 + (length - 3) / 4;
	else if (length < 67)
		return 269 + (length - 3) / 8;
	else if (length < 131)
		return 273 + (length - 3) / 16;
	else if (length < 258)
		return 277 + (length - 3) / 32;
	else
		return 285;
}

void isal_update_histogram_dict(uint8_t * start_stream, int dict_length, int length,
				struct isal_huff_histogram *histogram)
{
	uint32_t literal = 0, hash;
	uint16_t seen, *last_seen = histogram->hash_table;
	uint8_t *current, *end_stream, *next_hash, *end, *end_dict;
	uint32_t match_length;
	uint32_t dist;
	uint64_t *lit_len_histogram = histogram->lit_len_histogram;
	uint64_t *dist_histogram = histogram->dist_histogram;

	if (length <= 0)
		return;

	end_stream = start_stream + dict_length + length;
	end_dict = start_stream + dict_length;

	memset(last_seen, 0, sizeof(histogram->hash_table));	/* Initialize last_seen to be 0. */

	for (current = start_stream; current < end_dict - 4; current++) {
		literal = load_u32(current);
		hash = compute_hash(literal) & LVL0_HASH_MASK;
		last_seen[hash] = (current - start_stream) & 0xFFFF;
	}

	for (current = start_stream + dict_length; current < end_stream - 3; current++) {
		literal = load_u32(current);
		hash = compute_hash(literal) & LVL0_HASH_MASK;
		seen = last_seen[hash];
		last_seen[hash] = (current - start_stream) & 0xFFFF;
		dist = (current - start_stream - seen) & 0xFFFF;
		if (dist - 1 < D - 1) {
			assert(start_stream <= current - dist);
			match_length =
			    compare258(current - dist, current, end_stream - current);
			if (match_length >= SHORTEST_MATCH) {
				next_hash = current;
#ifdef ISAL_LIMIT_HASH_UPDATE
				end = next_hash + 3;
#else
				end = next_hash + match_length;
#endif
				if (end > end_stream - 3)
					end = end_stream - 3;
				next_hash++;
				for (; next_hash < end; next_hash++) {
					literal = load_u32(next_hash);
					hash = compute_hash(literal) & LVL0_HASH_MASK;
					last_seen[hash] = (next_hash - start_stream) & 0xFFFF;
				}

				dist_histogram[convert_dist_to_dist_sym(dist)] += 1;
				lit_len_histogram[convert_length_to_len_sym(match_length)] +=
				    1;
				current += match_length - 1;
				continue;
			}
		}
		lit_len_histogram[literal & 0xFF] += 1;
	}

	for (; current < end_stream; current++)
		lit_len_histogram[*current] += 1;

	lit_len_histogram[256] += 1;
	return;
}

int main(int argc, char *argv[])
{
	long int file_length;
	int argi = 1;
	uint8_t *stream = NULL;
	struct isal_hufftables hufftables;
	struct isal_huff_histogram histogram;
	struct isal_zstream tmp_stream;
	FILE *file = NULL;
	FILE *dict_file = NULL;
	FILE *hist_file = NULL;
	long int dict_file_length = 0;
	long int hist_file_length = 0;
	uint8_t *dict_stream = NULL;

	if (argc == 1) {
		printf("Error, no input file.\n");
		return 1;
	}

	if (argc > 3 && argv[1][0] == '-' && argv[1][1] == 'd') {
		dict_file = fopen(argv[2], "r");

		fseek(dict_file, 0, SEEK_END);
		dict_file_length = ftell(dict_file);
		fseek(dict_file, 0, SEEK_SET);
		dict_file_length -= ftell(dict_file);
		dict_stream = malloc(dict_file_length);
		if (dict_stream == NULL) {
			printf("Failed to allocate memory to read in dictionary file\n");
			fclose(dict_file);
			return 1;
		}
		if (fread(dict_stream, 1, dict_file_length, dict_file) != dict_file_length) {
			printf("Error occurred when reading dictionary file");
			fclose(dict_file);
			free(dict_stream);
			return 1;
		}
		isal_update_histogram(dict_stream, dict_file_length, &histogram);

		printf("Read %ld bytes of dictionary file %s\n", dict_file_length, argv[2]);
		argi += 2;
		fclose(dict_file);
		free(dict_stream);
	}

	if ((argc > argi + 1) && argv[argi][0] == '-' && argv[argi][1] == 'h') {
		hist_file = fopen(argv[argi + 1], "r+");
		fseek(hist_file, 0, SEEK_END);
		hist_file_length = ftell(hist_file);
		fseek(hist_file, 0, SEEK_SET);
		hist_file_length -= ftell(hist_file);
		if (hist_file_length > sizeof(histogram)) {
			printf("Histogram file too long\n");
			return 1;
		}
		if (fread(&histogram, 1, hist_file_length, hist_file) != hist_file_length) {
			printf("Error occurred when reading history file");
			fclose(hist_file);
			return 1;
		}
		fseek(hist_file, 0, SEEK_SET);

		printf("Read %ld bytes of history file %s\n", hist_file_length,
		       argv[argi + 1]);
		argi += 2;
	} else
		memset(&histogram, 0, sizeof(histogram));	/* Initialize histograms. */

	while (argi < argc) {
		printf("Processing %s\n", argv[argi]);
		file = fopen(argv[argi], "r");
		if (file == NULL) {
			printf("Error opening file\n");
			return 1;
		}
		fseek(file, 0, SEEK_END);
		file_length = ftell(file);
		fseek(file, 0, SEEK_SET);
		file_length -= ftell(file);
		stream = malloc(file_length + dict_file_length);
		if (stream == NULL) {
			printf("Failed to allocate memory to read in file\n");
			fclose(file);
			return 1;
		}
		if (dict_file_length > 0)
			memcpy(stream, dict_stream, dict_file_length);

		if (fread(&stream[dict_file_length], 1, file_length, file) != file_length) {
			printf("Error occurred when reading file");
			fclose(file);
			free(stream);
			return 1;
		}

		/* Create a histogram of frequency of symbols found in stream to
		 * generate the huffman tree.*/
		if (0 == dict_file_length)
			isal_update_histogram(stream, file_length, &histogram);
		else
			isal_update_histogram_dict(stream, dict_file_length, file_length,
						   &histogram);

		fclose(file);
		free(stream);
		argi++;
	}

	isal_create_hufftables(&hufftables, &histogram);

	file = fopen("hufftables_c.c", "w");
	if (file == NULL) {
		printf("Error creating file hufftables_c.c\n");
		return 1;
	}

	fprint_header(file);

	fprintf(file, "\n");

	fprint_hufftables(file, "hufftables_default", &hufftables);

	fprintf(file, "\n");

	isal_deflate_stateless_init(&tmp_stream);
	isal_deflate_set_hufftables(&tmp_stream, NULL, IGZIP_HUFFTABLE_STATIC);
	fprint_hufftables(file, "hufftables_static", tmp_stream.hufftables);

	fclose(file);

	if (hist_file) {
		int len = fwrite(&histogram, 1, sizeof(histogram), hist_file);
		printf("wrote %d bytes of histogram file\n", len);
		fclose(hist_file);
	}
	return 0;
}
