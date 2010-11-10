/*
 * ex_column.h Copyright (c) 2010 WiredTiger
 *
 * Declarations shared by ex_column*.c
 */

#include <inttypes.h>
#include <wt/wtds.h>

typedef struct {
	char country[5];
	int16_t year;
	int64_t population;
} POP_RECORD;
