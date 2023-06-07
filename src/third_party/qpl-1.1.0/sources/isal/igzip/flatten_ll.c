/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "flatten_ll.h"

void flatten_ll(uint32_t * ll_hist)
{
	uint32_t i, j;
	uint32_t *s = ll_hist, x, *p;

	s[265] += s[266];
	s[266] = s[267] + s[268];
	s[267] = s[269] + s[270];
	s[268] = s[271] + s[272];
	s[269] = s[273] + s[274] + s[275] + s[276];
	s[270] = s[277] + s[278] + s[279] + s[280];
	s[271] = s[281] + s[282] + s[283] + s[284];
	s[272] = s[285] + s[286] + s[287] + s[288];
	p = s + 289;
	for (i = 273; i < 277; i++) {
		x = *(p++);
		for (j = 1; j < 8; j++)
			x += *(p++);
		s[i] = x;
	}
	for (; i < 281; i++) {
		x = *(p++);
		for (j = 1; j < 16; j++)
			x += *(p++);
		s[i] = x;
	}
	for (; i < 285; i++) {
		x = *(p++);
		for (j = 1; j < 32; j++)
			x += *(p++);
		s[i] = x;
	}
	s[284] -= s[512];
	s[285] = s[512];
}
