/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/
 
#ifndef _IGZIP_REPEATED_8K_CHAR_RESULT_H_
#define _IGZIP_REPEATED_8K_CHAR_RESULT_H_

/* The code for the literal being encoded */
#define CODE_LIT 0x1
#define CODE_LIT_LENGTH 0x2

/* The code for repeat 10. The Length includes the distance code length*/
#define CODE_10  0x3
#define CODE_10_LENGTH  0x4

/* The code for repeat 115-130. The Length includes the distance code length*/
#define CODE_280 0x0f
#define CODE_280_LENGTH 0x4
#define CODE_280_TOTAL_LENGTH CODE_280_LENGTH + 4 + 1

/* Code representing the end of block. */
#define END_OF_BLOCK 0x7
#define END_OF_BLOCK_LEN 0x4

/* MIN_REPEAT_LEN currently optimizes storage space, another possiblity is to
 * find the size which optimizes speed instead.*/
#define MIN_REPEAT_LEN 4*1024

#define HEADER_LENGTH 16

/* Maximum length of the portion of the header represented by repeat lengths
 * smaller than 258 */
#define MAX_FIXUP_CODE_LENGTH 8


/* Headers for constant 0x00 and 0xFF blocks
 * This also contains the first literal character. */
const uint32_t repeated_char_header[2][5] = {
	{ 0x0121c0ec, 0xc30c0000, 0x7d57fab0, 0x49270938}, /* Deflate header for 0x00 */
	{ 0x0121c0ec, 0xc30c0000, 0x7baaff30, 0x49270938}  /* Deflate header for 0xFF */

};

#endif /*_IGZIP_REPEATED_8K_CHAR_RESULT_H_*/
