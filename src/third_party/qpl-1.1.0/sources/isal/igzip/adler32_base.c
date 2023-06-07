/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdint.h>
#include "igzip_checksums.h"

uint32_t adler32_base(uint32_t adler32, uint8_t * start, uint32_t length)
{
	uint8_t *end, *next = start;
	uint64_t A, B;

	A = adler32 & 0xffff;
	B = adler32 >> 16;

	while (length > MAX_ADLER_BUF) {
		end = next + MAX_ADLER_BUF;
		for (; next < end; next++) {
			A += *next;
			B += A;
		}

		A = A % ADLER_MOD;
		B = B % ADLER_MOD;
		length -= MAX_ADLER_BUF;
	}

	end = next + length;
	for (; next < end; next++) {
		A += *next;
		B += A;
	}

	A = A % ADLER_MOD;
	B = B % ADLER_MOD;

	return B << 16 | A;
}
