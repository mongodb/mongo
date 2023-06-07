/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include "igzip_lib.h"

#define STATIC_INFLATE_FILE "static_inflate.h"
#define DOUBLE_SYM_THRESH (4 * 1024)

extern struct isal_hufftables hufftables_default;

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

void fprint_header(FILE * output_file)
{
	fprintf(output_file, "#include \"igzip_lib.h\"\n\n");
	fprintf(output_file, "#define LONG_BITS_CHECK %d\n", ISAL_DECODE_LONG_BITS);
	fprintf(output_file, "#define SHORT_BITS_CHECK %d\n", ISAL_DECODE_SHORT_BITS);
	fprintf(output_file,
		"#if (LONG_BITS_CHECK == ISAL_DECODE_LONG_BITS) && (SHORT_BITS_CHECK == ISAL_DECODE_SHORT_BITS)\n"
		"# define ISAL_STATIC_INFLATE_TABLE\n"
		"#else\n"
		"# warning \"Incompatible compile time defines for optimized static inflate table.\"\n"
		"#endif\n\n");
}

int main(int argc, char *argv[])
{
	struct inflate_state state;
	FILE *file;
	uint8_t static_deflate_hdr = 3;
	uint8_t tmp_space[8], *in_buf;

	if (NULL == (in_buf = malloc(DOUBLE_SYM_THRESH + 1))) {
		printf("Can not allocote memory\n");
		return 1;
	}

	isal_inflate_init(&state);

	memcpy(in_buf, &static_deflate_hdr, sizeof(static_deflate_hdr));
	state.next_in = in_buf;
	state.avail_in = DOUBLE_SYM_THRESH + 1;
	state.next_out = tmp_space;
	state.avail_out = sizeof(tmp_space);

	isal_inflate(&state);

	file = fopen(STATIC_INFLATE_FILE, "w");

	if (file == NULL) {
		printf("Error creating file hufftables_c.c\n");
		return 1;
	}
	// Add decode tables describing a type 2 static (fixed) header

	fprintf(file, "#ifndef STATIC_HEADER_H\n" "#define STATIC_HEADER_H\n\n");

	fprint_header(file);

	fprintf(file, "struct inflate_huff_code_large static_lit_huff_code = {\n");
	fprint_uint32_table(file, state.lit_huff_code.short_code_lookup,
			    sizeof(state.lit_huff_code.short_code_lookup) / sizeof(uint32_t),
			    "\t.short_code_lookup = {", "\t},\n\n", "\t\t");
	fprint_uint16_table(file, state.lit_huff_code.long_code_lookup,
			    sizeof(state.lit_huff_code.long_code_lookup) / sizeof(uint16_t),
			    "\t.long_code_lookup = {", "\t}\n", "\t\t");
	fprintf(file, "};\n\n");

	fprintf(file, "struct inflate_huff_code_small static_dist_huff_code = {\n");
	fprint_uint16_table(file, state.dist_huff_code.short_code_lookup,
			    sizeof(state.dist_huff_code.short_code_lookup) / sizeof(uint16_t),
			    "\t.short_code_lookup = {", "\t},\n\n", "\t\t");
	fprint_uint16_table(file, state.dist_huff_code.long_code_lookup,
			    sizeof(state.dist_huff_code.long_code_lookup) / sizeof(uint16_t),
			    "\t.long_code_lookup = {", "\t}\n", "\t\t");
	fprintf(file, "};\n\n");

	fprintf(file, "#endif\n");

	// Add other tables for known dynamic headers - level 0

	isal_inflate_init(&state);

	memcpy(in_buf, &hufftables_default.deflate_hdr,
	       sizeof(hufftables_default.deflate_hdr));
	state.next_in = in_buf;
	state.avail_in = DOUBLE_SYM_THRESH + 1;
	state.next_out = tmp_space;
	state.avail_out = sizeof(tmp_space);

	isal_inflate(&state);

	fprintf(file, "struct inflate_huff_code_large pregen_lit_huff_code = {\n");
	fprint_uint32_table(file, state.lit_huff_code.short_code_lookup,
			    sizeof(state.lit_huff_code.short_code_lookup) / sizeof(uint32_t),
			    "\t.short_code_lookup = {", "\t},\n\n", "\t\t");
	fprint_uint16_table(file, state.lit_huff_code.long_code_lookup,
			    sizeof(state.lit_huff_code.long_code_lookup) / sizeof(uint16_t),
			    "\t.long_code_lookup = {", "\t}\n", "\t\t");
	fprintf(file, "};\n\n");

	fprintf(file, "struct inflate_huff_code_small pregen_dist_huff_code = {\n");
	fprint_uint16_table(file, state.dist_huff_code.short_code_lookup,
			    sizeof(state.dist_huff_code.short_code_lookup) / sizeof(uint16_t),
			    "\t.short_code_lookup = {", "\t},\n\n", "\t\t");
	fprint_uint16_table(file, state.dist_huff_code.long_code_lookup,
			    sizeof(state.dist_huff_code.long_code_lookup) / sizeof(uint16_t),
			    "\t.long_code_lookup = {", "\t}\n", "\t\t");
	fprintf(file, "};\n\n");

	fclose(file);
	free(in_buf);
	return 0;
}
