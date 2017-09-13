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
 * Example that shows how to convert a string and TZ identifier to its parts,
 * in both normal and ISO week date parts.
 *
 * Compile with:
 * gcc -ggdb3 -o date-to-parts date-to-parts.c ../timelib.a -lm
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
	char           *dt_string  = "2017-06-05T11:30:09.123Z";
	char           *tz_id      = "Europe/London";
	timelib_time   *t;
	timelib_tzinfo *tzi;
	timelib_error_container *errors;

	create_cache((timelib_tzdb*) timelib_builtin_db());

	tzi = cached_fetch_tzinfo(tz_id);

	/* Convert string to timelib_time, and hence its consitituent parts */
	t = timelib_strtotime(
		dt_string, strlen(dt_string),
		&errors,
		global.db,
		cached_tzfile_wrapper
	);
	timelib_update_ts(t, tzi);
	timelib_set_timezone(t, tzi);
	timelib_unixtime2local(t, t->sse);


#define LLABS(y) (y < 0 ? (y * -1) : y)

	/* Show parts Y/m/d */
	{
		printf(
			"%s%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
			t->y < 0 ? "-" : "", LLABS(t->y),
			t->m, t->d, t->h, t->i, t->s
		);
		if (t->us > 0) {
			printf(".%03lld", (t->us / 1000));
		}
		printf("\n");
	}


	/* Show parts ISO */
	{
		timelib_sll iso_year, iso_week, iso_dow;

		timelib_isodate_from_date(t->y, t->m, t->d, &iso_year, &iso_week, &iso_dow);
		printf(
			"%s%04lldW%02lldD%02lld %02lld:%02lld:%02lld",
			iso_year < 0 ? "-" : "", LLABS(iso_year),
			iso_week, iso_dow,
			t->h, t->i, t->s
		);
		if (t->us > 0) {
			printf(".%03lld", (t->us / 1000));
		}
		printf("\n");
	}

	timelib_error_container_dtor(errors);
	timelib_tzinfo_dtor(tzi);
	timelib_time_dtor(t);

	cleanup_cache();
}
