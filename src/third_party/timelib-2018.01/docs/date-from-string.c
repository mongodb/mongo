/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 MongoDB, Inc.
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
 * gcc -ggdb3 -o date-from-string date-from-string.c ../timelib.a -lm
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
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

int main(int argc, char *argv[])
{
	char           *time_string = NULL;
	char           *tz_id = NULL;
	timelib_time   *t;
#if defined(PARTIAL)
	timelib_time   *t_now;
#endif
	timelib_tzinfo *tzi = NULL;
	timelib_tzinfo *tzi_utc = NULL;
	timelib_error_container *errors;
	
	if (argc < 2) {
		printf("Usage:\n\tdate-from-string string [tzExpression]\n\n");
		exit(-1);
	}
	
	time_string = argv[1];

	if (argc >= 3) {
		tz_id = argv[2];
	}

	create_cache((timelib_tzdb*) timelib_builtin_db());

	if (tz_id) {
		tzi = cached_fetch_tzinfo(tz_id);
	}
	tzi_utc = cached_fetch_tzinfo("UTC");

#if defined(PARTIAL)
	/* Doing this means you can also supply partial date/time strings, such as
	 * "July 4th" */

	/* Create "now" to fill in the holes */
	t_now = timelib_time_ctor();
	if (tzi) {
		timelib_set_timezone(t_now, tzi);
	} else {
		timelib_set_timezone(t_now, tzi_utc);
	}
	timelib_unixtime2gmt(t_now, time(NULL));
#endif

	/* Convert from string to timelib_t
	 *
	 * Passing in the "Z" at the end of the string, means the extra timezone gets ignored.
	 * If you *don't* want that, then compile with -DDONT_IGNORE_TZ */
	t = timelib_strtotime(time_string, strlen(time_string), &errors, global.db, cached_tzfile_wrapper);

	/* Error handling */
	if (errors->warning_count) {
		printf("Warnings found while parsing '%s'\n", time_string);
	}
	if (errors->error_count) {
		printf("Errors found while parsing '%s'\n", time_string);
	}
	timelib_error_container_dtor(errors);

#if defined(PARTIAL)
	/* Add missing fields */
	timelib_fill_holes(t, t_now, TIMELIB_NO_CLONE);
#endif

	if (tzi) {
#if defined(DONT_IGNORE_TZ)
		timelib_set_timezone(t, tzi);
#endif

		timelib_update_ts(t, tzi);
		timelib_unixtime2local(t, t->sse);
		timelib_dump_date(t, 1);
	} else {
		timelib_update_ts(t, tzi_utc);
		timelib_dump_date(t, 1);
	}

	timelib_set_timezone(t, tzi_utc);
	timelib_unixtime2local(t, t->sse);

	/* Show Unix timestamp */
	timelib_dump_date(t, 1);
	printf("Timestamp: %lld\n", (t->sse * 1000) + (int) (t->us / 1000.0));

	timelib_time_dtor(t);
#if defined(PARTIAL)
	timelib_time_dtor(t_now);
#endif
	if (tzi) {
		timelib_tzinfo_dtor(tzi);
	}
	timelib_tzinfo_dtor(tzi_utc);

	cleanup_cache();
}
