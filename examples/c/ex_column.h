/*
 * ex_column.h Copyright (c) 2010 WiredTiger
 *
 * Declarations shared by ex_column*.c
 */

#include <inttypes.h>
#include <wt/wtds.h>

typedef struct {
	char country[5];
	uint16_t year;
	uint64_t population;
} POP_RECORD;
