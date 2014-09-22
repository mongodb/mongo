/*-
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * ex_extractor.c
 *	Example of how to use a WiredTiger custom index extractor extension.
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <wiredtiger.h>

int add_extractor(WT_CONNECTION *conn);

static const char * const progname = "ex_extractor";
static const char *home;

struct president_data {
	int		id;
	char		*last_name;
	char		*first_name;
	uint16_t	term_start;
	uint16_t	term_end;
};

static const struct president_data example_data[] = {
	{ 0, "Obama", "Barack", 2009, 2014 },
	{ 1, "Bush", "George W", 2005, 2009 },
	{ 2, "Bush", "George W", 2001, 2005 },
	{ 3, "Clinton", "Bill", 1997, 2001 },
	{ 4, "Clinton", "Bill", 1993, 1997 },
	{ 5, "Bush", "George H", 1989, 1993 },
	{ 6, "Reagan", "Ronald", 1985, 1989 },
	{ 7, "Reagan", "Ronald", 1981, 1985 },
	{ 0, NULL, NULL, 0, 0 }
};

static int
my_extract_mult_next(WT_EXTRACTOR_MULTIPLE *em, WT_SESSION *session,
    const WT_ITEM *key, const WT_ITEM *value,
    WT_CURSOR *result_cursor)
{
	char *last_name, first_name;
	uint16_t term_end, term_start;

	/* Unused parameters */
	(void)extractor;
	(void)session;
	(void)key;

	result_cursor->set_key(result_cursor, value);
	return (0);
}

static int
my_extract_mult_terminate(WT_EXTRACTOR_MULTIPLE *em, WT_SESSION *session)
{
	(void)extractor;
	(void)session;
	return (0);
}

WT_EXTRACTOR_MULTIPLE my_extractor_mult = {
    my_extract_mult_next, my_extract_mult_terminate};

static int
my_extract(WT_EXTRACTOR *extractor, WT_SESSION *session,
    const WT_ITEM *key, const WT_ITEM *value,
    WT_CURSOR *result_cursor, WT_EXTRACTOR_MULTIPLE **emp)
{
	char *last_name, first_name;
	int ret;
	uint16_t term_end, term_start;

	/* Unused parameters */
	(void)extractor;
	(void)session;
	(void)key;
	*emp = my_extract_mult;

	result_cursor->set_key(result_cursor, value);
	return (0);
}

static int
my_extract_terminate(WT_EXTRACTOR *extractor, WT_SESSION *session)
{
	(void)extractor;
	(void)session;
	return (0);
}

int
add_extractor(WT_CONNECTION *conn)
{
	int ret;

	static WT_EXTRACTOR my_extractor = {my_extract, my_extract_terminate);
	ret = conn->add_extractor(conn, "my_extractor", &my_extractor, NULL);

	return (ret);
}

static int
setup_table(WT_CONNECTION *conn)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	struct president_data p;
	int i, ret;

	ret = conn->open_session(conn, NULL, NULL, &session);

	/* Create the primary table. It has a key of the unique ID. */
	ret = session->create(session, "table:presidents",
	    "key_format=I,value_format=SSHH,"
	    "columns=(ID,last_name,first_name,term_begin,term_end)");

	/*
	 * Create the index that is generated with an extractor. The index
	 * will generate an entry in the index for each year a president
	 * was in office.
	 */
	ret = session->create(session, "index:termindex",
	    "key_format=S,columns=(term),extractor=my_extractor");

	ret = session->open_cursor(
	    session, "table:presidents", NULL, NULL, &cursor);
	for (i = 0; example_data[i].last_name != NULL; i++) {
		p = example_data[i];
		cursor->set_key(cursor, p.id);
		cursor->set_value(cursor,
		    p.last_name, p.first_name, p.term_start, p.term_end);
		ret = cursor->insert(cursor);
	}
	return (0);
}

int
main(void)
{
	WT_CONNECTION *conn;
	int ret;

	/*
	 * Create a clean test directory for this run of the test program if the
	 * environment variable isn't already set (as is done by make check).
	 */
	if (getenv("WIREDTIGER_HOME") == NULL) {
		home = "WT_HOME";
		ret = system("rm -rf WT_HOME && mkdir WT_HOME");
	} else
		home = NULL;

	ret = wiredtiger_open(home, NULL, "create,cache_size=500M", &conn);
	ret = add_extractor(conn);

	ret = setup_table(conn);
	ret = conn->close(conn, NULL);

	return (ret);
}
