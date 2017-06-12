CFLAGS=-O0 -ggdb3 \
	-Wall -Werror -Wextra -fsanitize=undefined \
	-Wmaybe-uninitialized -Wdeclaration-after-statement -Wmissing-field-initializers -Wshadow -Wno-unused-parameter \
	-pedantic \
	-DHAVE_STDINT_H -DHAVE_STRING_H -DHAVE_GETTIMEOFDAY -DHAVE_UNISTD_H -DHAVE_DIRENT_H -I.# -DDEBUG_PARSER

LDFLAGS=-lm -fsanitize=undefined
CC=gcc
MANUAL_TESTS=tests/tester-parse-interval \
	tests/tester-parse-tz tests/tester-iso-week tests/test-abbr-to-id \
	tests/enumerate-timezones tests/date_from_isodate
AUTO_TESTS=tests/tester-parse-string tests/tester-parse-string-by-format \
	tests/tester-create-ts tests/tester-render-ts tests/tester-render-ts-zoneinfo
TEST_BINARIES=${MANUAL_TESTS} ${AUTO_TESTS}

all: parse_date.o tm2unixtime.o unixtime2tm.o dow.o astro.o interval.o \
		${TEST_BINARIES}

parse_date.c: timezonemap.h parse_date.re
	re2c -d -b parse_date.re > parse_date.c

parse_iso_intervals.c: parse_iso_intervals.re
	re2c -d -b parse_iso_intervals.re > parse_iso_intervals.c

timelib.a: parse_iso_intervals.o parse_date.o unixtime2tm.o tm2unixtime.o dow.o parse_tz.o parse_zoneinfo.o timelib.o astro.o interval.o
	ar -rc timelib.a parse_iso_intervals.o parse_date.o unixtime2tm.o tm2unixtime.o dow.o parse_tz.o parse_zoneinfo.o timelib.o astro.o interval.o

tests/tester-diff: timelib.a tests/tester-diff.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/tester-diff tests/tester-diff.c timelib.a

tests/tester-parse-string: timelib.a tests/tester-parse-string.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/tester-parse-string tests/tester-parse-string.c timelib.a

tests/tester-parse-interval: timelib.a tests/tester-parse-interval.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/tester-parse-interval tests/tester-parse-interval.c timelib.a

tests/tester-parse-string-by-format: timelib.a tests/tester-parse-string-by-format.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/tester-parse-string-by-format tests/tester-parse-string-by-format.c timelib.a

tests/tester-create-ts: timelib.a tests/tester-create-ts.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/tester-create-ts tests/tester-create-ts.c timelib.a

tests/tester-parse-tz: timelib.a tests/test-tz-parser.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/tester-parse-tz tests/test-tz-parser.c timelib.a

tests/tester-render-ts: timelib.a tests/tester-render-ts.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/tester-render-ts tests/tester-render-ts.c timelib.a

tests/tester-render-ts-zoneinfo: timelib.a tests/tester-render-ts-zoneinfo.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/tester-render-ts-zoneinfo tests/tester-render-ts-zoneinfo.c timelib.a

tests/tester-iso-week: timelib.a tests/tester-iso-week.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/tester-iso-week tests/tester-iso-week.c timelib.a

tests/test-abbr-to-id: timelib.a tests/test-abbr-to-id.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/test-abbr-to-id tests/test-abbr-to-id.c timelib.a

tests/test-astro: timelib.a tests/test-astro.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/test-astro tests/test-astro.c timelib.a -lm

tests/enumerate-timezones: timelib.a tests/enumerate-timezones.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/enumerate-timezones tests/enumerate-timezones.c timelib.a

tests/date_from_isodate: timelib.a tests/date_from_isodate.c
	gcc $(CFLAGS) $(LDFLAGS) -o tests/date_from_isodate tests/date_from_isodate.c timelib.a

timezonemap.h: gettzmapping.php
	echo Generating timezone mapping file.
	php gettzmapping.php > timezonemap.h

clean-all: clean
	rm -f timezonemap.h

clean:
	rm -f parse_iso_intervals.c parse_date.c *.o timelib.a ${TEST_BINARIES}

test: tests/tester-parse-string tests/tester-create-ts tests/tester-render-ts tests/tester-render-ts-zoneinfo tests/tester-parse-string-by-format
	php tests/test_all.php

test-parse-string: tests/tester-parse-string
	@for i in tests/files/*.parse; do echo $$i; php tests/test_parser.php $$i; echo; done

test-parse-format: tests/tester-parse-string-by-format
	@for i in tests/files/*.parseformat; do echo $$i; php tests/test_parse_format.php $$i; echo; done

test-create-ts: tests/tester-create-ts
	@for i in tests/files/*.ts; do echo $$i; php tests/test_create.php $$i; echo; done

test-render-ts: tests/tester-render-ts
	@for i in tests/files/*.render; do echo $$i; php tests/test_render.php $$i; echo; done

test-render-ts-zoneinfo: tests/tester-render-ts-zoneinfo
	@for i in tests/files/*.render; do echo $$i; php tests/test_render.php $$i; echo; done

package: clean
	tar -cvzf parse_date.tar.gz parse_date.re Makefile tests
