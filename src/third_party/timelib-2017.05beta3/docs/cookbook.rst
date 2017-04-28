========
Cookbook
========

Converting a DateTime string with timezone to parts
---------------------------------------------------

::
	cache *tz_cache;

::
	create_cache(timelib_tzdb *db)
	{
		/* Loop over all the entries and store in cache */
	}

::
	cleanup_cache()
	{
		/* Loop over all the entries and store in cache */
	}

::
	timelib_tzinfo *cached_tzfile_wrapper(char *tzid)
	{
		return tz_cache[tzid]; /* pseudo code */
	}

::
	int main(void)
	{
		char           *dt_string  = "2017-06-05T11:30:09.123Z";
		char           *tz_id      = "Europe/London";
		timelib_tzdb   *db         = timelib_builtin_db();
		timelib_time   *t;
		timelib_tzinfo *tzi;

		tz_cache = create_cache();

		tzi = cached_fetch_tzinfo(tz_id);

		t = timelib_strtotime(
			dt_string, sizeof(dt_string) - 1,
			&errors,
			db,
			cached_tzfile_wrapper
		);

		timelib_set_timezone(t, tzi);
		timelib_unixtime2local(t, t->sse);


		/* Show parts Y/m/d */
		{
			printf(
				"%s%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
				t->y < 0 ? "-" : "", TIMELIB_LLABS(t->y),
				t->m, t->d, t->h, t->i, t->s
			);
			if (t->f > +0.0) {
				printf(" %.6f", t->f);
			}
		}


		/* Show parts ISO */
		{
			timelib_sll iso_year, iso_week, iso_dow;

			timelib_isodate_from_date(t->y, t->m, t->d, &iso_year, &iso_week, &iso_dow);
			printf(
				"%s%04lldW%02lldD%02lld %02lld:%02lld:%02lld",
				iso_year < 0 ? "-" : "", TIMELIB_LLABS(iso_year),
				iso_week, iso_dow,
				t->h, t->i, t->s
			);
			if (t->f > +0.0) {
				printf(" %.6f", t->f);
			}
		}


		timelib_time_dtor(t);

		if (db != timelib_builtin_db()) {
			timelib_tzdb_dtor(db);
		}

		cleanup_cache();
	}
