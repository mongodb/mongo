/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Derick Rethans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Example that shows how to convert a date/time in its parts, to a 
 * Unix timestamp.
 *
 * Compile with:
 * gcc -ggdb3 -o date-from-parts date-from-parts.c ../timelib.a -lm
 */

#include <stdio.h>
#include <string.h>
#include "../timelib.h"

struct {
	timelib_tzdb *db;
	/* cache *tz_cache; */
} global;

void create_cache(timelib_tzdb *db)
{
	global.db = db;

	/* Loop over all the entries and store in tz_cache */
}

void cleanup_cache()
{
	if (global.db != timelib_builtin_db()) {
		timelib_zoneinfo_dtor(global.db);
	}

	/* Loop over all the entries in tz_cache and free */
}

timelib_tzinfo *cached_tzfile_wrapper(char *tz_id, const timelib_tzdb *db, int *error)
{
	/* return tz_cache[tzid]; (pseudo code) */
	return timelib_parse_tzfile(tz_id, global.db, error);
}

timelib_tzinfo *cached_fetch_tzinfo(char *tz_id)
{
	int dummy_error;

	return cached_tzfile_wrapper(tz_id, global.db, &dummy_error);
}

int main(void)
{
	timelib_sll ty = 2017;
	timelib_sll tm = 6;
	timelib_sll td = 6;
	timelib_sll th = 12;
	timelib_sll ti = 50;
	timelib_sll ts = 58;
	timelib_sll tus = 713 * 1000;
	char           *tz_id = "America/New_York";
	timelib_time   *t;
	timelib_tzinfo *tzi;

	create_cache((timelib_tzdb*) timelib_builtin_db());

	tzi = cached_fetch_tzinfo(tz_id);

	t = timelib_time_ctor();
	t->y = ty; t->m = tm; t->d = td;
	t->h = th; t->i = ti; t->s = ts;
	t->us = tus;

	timelib_update_ts(t, tzi);
	timelib_set_timezone(t, tzi);
	timelib_unixtime2gmt(t, t->sse); /* Note it says gmt in the function name */


#define LLABS(y) (y < 0 ? (y * -1) : y)

	/* Show parts Y/m/d */
	{
		printf(
			"%s%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
			t->y < 0 ? "-" : "", LLABS(t->y),
			t->m, t->d, t->h, t->i, t->s
		);
		if (t->us > 0) {
			printf(".%06lld", t->us);
		}
		printf("\n");
	}


	/* Show Unix timestamp */
	{
		printf("Timestamp: %lld\n", t->sse);
	}


	timelib_tzinfo_dtor(tzi);
	timelib_time_dtor(t);

	cleanup_cache();
}
