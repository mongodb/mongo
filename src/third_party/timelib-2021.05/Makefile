FLAGS=-O0 -ggdb3 \
	-Wall -Werror -Wextra -fsanitize=undefined -fsanitize=address \
	-Wmaybe-uninitialized -Wmissing-field-initializers -Wshadow -Wno-unused-parameter \
	-pedantic -Wno-implicit-fallthrough \
	-DHAVE_STDINT_H -DHAVE_GETTIMEOFDAY -DHAVE_UNISTD_H -DHAVE_DIRENT_H -I.# -DDEBUG_PARSER

CFLAGS=-Wdeclaration-after-statement ${FLAGS}

CPPFLAGS=${FLAGS}

LDFLAGS=-lm -fsanitize=undefined -l:libubsan.so

TEST_LDFLAGS=-lCppUTest

CC=gcc
CXX=g++

MANUAL_TESTS=tests/tester-parse-interval \
	tests/tester-iso-week tests/test-abbr-to-id \
	tests/enumerate-timezones tests/date_from_isodate
AUTO_TESTS=tests/tester-parse-string tests/tester-parse-string-by-format \
	tests/tester-create-ts tests/tester-render-ts tests/tester-render-ts-zoneinfo
C_TESTS=tests/c/timelib_get_current_offset_test.o tests/c/timelib_decimal_hour.o \
	tests/c/timelib_juliandate.o tests/c/issues.o tests/c/astro_rise_set_altitude.o \
	tests/c/parse_date_from_format_test.o tests/c/parse_intervals.o \
	tests/c/warn_on_slim.o tests/c/parse_posix.o tests/c/transitions.o \
	tests/c/parse_tz.o tests/c/render.o tests/c/create_ts_from_string.o \
	tests/c/parse_date.o tests/c/php-rfc.o tests/c/diff.cpp

TEST_BINARIES=${MANUAL_TESTS} ${AUTO_TESTS}

EXAMPLE_BINARIES=docs/date-from-iso-parts docs/date-from-parts docs/date-from-string \
	docs/date-to-parts docs/show-tzinfo

all: parse_date.o tm2unixtime.o unixtime2tm.o dow.o astro.o interval.o \
		${TEST_BINARIES} ${EXAMPLE_BINARIES} ctest

parse_date.c: timezonemap.h parse_date.re
	re2c -d -b parse_date.re > parse_date.c

parse_iso_intervals.c: parse_iso_intervals.re
	re2c -d -b parse_iso_intervals.re > parse_iso_intervals.c

timelib.a: parse_iso_intervals.o parse_date.o unixtime2tm.o tm2unixtime.o dow.o parse_tz.o parse_zoneinfo.o timelib.o astro.o interval.o parse_posix.o
	ar -rc timelib.a parse_iso_intervals.o parse_date.o unixtime2tm.o tm2unixtime.o dow.o parse_tz.o parse_zoneinfo.o timelib.o astro.o interval.o parse_posix.o

tests/tester-diff: timelib.a tests/tester-diff.c
	$(CC) $(CFLAGS) -o tests/tester-diff tests/tester-diff.c timelib.a $(LDFLAGS)

tests/tester-parse-string: timelib.a tests/tester-parse-string.c
	$(CC) $(CFLAGS) -o tests/tester-parse-string tests/tester-parse-string.c timelib.a $(LDFLAGS)

tests/tester-parse-interval: timelib.a tests/tester-parse-interval.c
	$(CC) $(CFLAGS) -o tests/tester-parse-interval tests/tester-parse-interval.c timelib.a $(LDFLAGS)

tests/tester-parse-string-by-format: timelib.a tests/tester-parse-string-by-format.c
	$(CC) $(CFLAGS) -o tests/tester-parse-string-by-format tests/tester-parse-string-by-format.c timelib.a $(LDFLAGS)

tests/tester-create-ts: timelib.a tests/tester-create-ts.c
	$(CC) $(CFLAGS) -o tests/tester-create-ts tests/tester-create-ts.c timelib.a $(LDFLAGS)

tests/tester-render-ts: timelib.a tests/tester-render-ts.c
	$(CC) $(CFLAGS) -o tests/tester-render-ts tests/tester-render-ts.c timelib.a $(LDFLAGS)

tests/tester-render-ts-zoneinfo: timelib.a tests/tester-render-ts-zoneinfo.c
	$(CC) $(CFLAGS) -o tests/tester-render-ts-zoneinfo tests/tester-render-ts-zoneinfo.c timelib.a $(LDFLAGS)

tests/tester-iso-week: timelib.a tests/tester-iso-week.c
	$(CC) $(CFLAGS) -o tests/tester-iso-week tests/tester-iso-week.c timelib.a $(LDFLAGS)

tests/test-abbr-to-id: timelib.a tests/test-abbr-to-id.c
	$(CC) $(CFLAGS) -o tests/test-abbr-to-id tests/test-abbr-to-id.c timelib.a $(LDFLAGS)

tests/test-astro: timelib.a tests/test-astro.c
	$(CC) $(CFLAGS) -o tests/test-astro tests/test-astro.c timelib.a -lm $(LDFLAGS)

tests/enumerate-timezones: timelib.a tests/enumerate-timezones.c
	$(CC) $(CFLAGS) -o tests/enumerate-timezones tests/enumerate-timezones.c timelib.a $(LDFLAGS)

tests/date_from_isodate: timelib.a tests/date_from_isodate.c
	$(CC) $(CFLAGS) -o tests/date_from_isodate tests/date_from_isodate.c timelib.a $(LDFLAGS)


docs/date-from-parts: timelib.a docs/date-from-parts.c
	$(CC) $(CFLAGS) -o docs/date-from-parts docs/date-from-parts.c timelib.a $(LDFLAGS)

docs/date-from-iso-parts: timelib.a docs/date-from-iso-parts.c
	$(CC) $(CFLAGS) -o docs/date-from-iso-parts docs/date-from-iso-parts.c timelib.a $(LDFLAGS)

docs/date-from-string: timelib.a docs/date-from-string.c
	$(CC) $(CFLAGS) -o docs/date-from-string docs/date-from-string.c timelib.a $(LDFLAGS)

docs/date-to-parts: timelib.a docs/date-to-parts.c
	$(CC) $(CFLAGS) -o docs/date-to-parts docs/date-to-parts.c timelib.a $(LDFLAGS)

docs/show-tzinfo: timelib.a docs/show-tzinfo.c
	$(CC) $(CFLAGS) -o docs/show-tzinfo docs/show-tzinfo.c timelib.a $(LDFLAGS)


timezonemap.h: gettzmapping.php
	echo Generating timezone mapping file.
	php gettzmapping.php > timezonemap.h

clean-all: clean
	rm -f timezonemap.h

clean:
	rm -f parse_iso_intervals.c parse_date.c *.o tests/c/*.o timelib.a ${TEST_BINARIES}

#%.o: %.cpp timelib.a
#	$(CXX) -c $(CPPFLAGS) $(LDFLAGS) $< -o $@

ctest: tests/c/all_tests.cpp timelib.a ${C_TESTS}
	$(CXX) $(CPPFLAGS) $(LDFLAGS) tests/c/all_tests.cpp ${C_TESTS} timelib.a $(TEST_LDFLAGS) -o ctest

test: ctest
	@./ctest -c

package: clean
	tar -cvzf parse_date.tar.gz parse_date.re Makefile tests
