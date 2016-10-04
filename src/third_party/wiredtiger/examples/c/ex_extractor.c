/*-
 * Public Domain 2014-2016 MongoDB, Inc.
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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wiredtiger.h>

#define	RET_OK(ret)	((ret) == 0 || (ret) == WT_NOTFOUND)

int add_extractor(WT_CONNECTION *conn);

static const char *home;

struct president_data {
	int		id;
	const char	*last_name;
	const char	*first_name;
	uint16_t	term_start;
	uint16_t	term_end;
};

static const struct president_data example_data[] = {
	{ 0, "Obama", "Barack", 2009, 2014 },
	{ 1, "Bush", "George W", 2001, 2009 },
	{ 2, "Clinton", "Bill", 1993, 2001 },
	{ 3, "Bush", "George H", 1989, 1993 },
	{ 4, "Reagan", "Ronald", 1981, 1989 },
	{ 0, NULL, NULL, 0, 0 }
};
/*
 * Number of years this data spans
 */
#define	YEAR_BASE	1981
#define	YEAR_SPAN	(2014-1981)

/*
 * A custom index extractor function that adds an index entry for each year of
 * the given president's term.
 */
static int
my_extract(WT_EXTRACTOR *extractor, WT_SESSION *session,
    const WT_ITEM *key, const WT_ITEM *value, WT_CURSOR *result_cursor)
{
	char *last_name, *first_name;
	uint16_t term_end, term_start, year;
	int ret;

	/* Unused parameters */
	(void)extractor;
	(void)key;

	/* Unpack the value. */
	if ((ret = wiredtiger_struct_unpack(
	    session, value->data, value->size, "SSHH",
	    &last_name, &first_name, &term_start, &term_end)) != 0)
		return (ret);

	/*
	 * We have overlapping years, so multiple records may share the same
	 * index key.
	 */
	for (year = term_start; year <= term_end; ++year) {
		/*
		 * Note that the extract callback is called for all operations
		 * that update the table, not just inserts.  The user sets the
		 * key and uses the cursor->insert() method to return the index
		 * key(s).  WiredTiger will perform the required operation
		 * (such as a remove()).
		 */
		fprintf(stderr,
		    "EXTRACTOR: index op for year %" PRIu16 ": %s %s\n",
		    year, first_name, last_name);
		result_cursor->set_key(result_cursor, year);
		if ((ret = result_cursor->insert(result_cursor)) != 0) {
			fprintf(stderr,
			    "EXTRACTOR: op year %" PRIu16 ": error %d\n",
			    year, ret);
			return (ret);
		}
	}
	return (0);
}

/*
 * The terminate method is called to release any allocated resources when the
 * table is closed.  In this example, no cleanup is required.
 */
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

	static WT_EXTRACTOR my_extractor = {
	    my_extract, NULL, my_extract_terminate
	};
	ret = conn->add_extractor(conn, "my_extractor", &my_extractor, NULL);

	return (ret);
}

/*
 * Read the index by year and print out who was in office that year.
 */
static int
read_index(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int i, ret;
	char *first_name, *last_name;
	uint16_t rec_year, term_end, term_start, year;

	year = 0;
	srand((unsigned int)getpid());
	ret = session->open_cursor(
	    session, "index:presidents:term", NULL, NULL, &cursor);
	/*
	 * Pick 10 random years and read the data.
	 */
	for (i = 0; i < 10 && RET_OK(ret); i++) {
		year = (uint16_t)((rand() % YEAR_SPAN) + YEAR_BASE);
		printf("Year %" PRIu16 ":\n", year);
		cursor->set_key(cursor, year);
		if ((ret = cursor->search(cursor)) != 0)
			break;
		if ((ret = cursor->get_key(cursor, &rec_year)) != 0)
			break;
		if ((ret = cursor->get_value(cursor,
		    &last_name, &first_name, &term_start, &term_end)) != 0)
			break;

		/* Report all presidents that served during the chosen year */
		while (term_start <= year &&
		    year <= term_end && year == rec_year) {
			printf("\t%s %s\n", first_name, last_name);
			if ((ret = cursor->next(cursor)) != 0)
				break;
			if ((ret = cursor->get_key(cursor, &rec_year)) != 0)
				break;
			if ((ret = cursor->get_value(cursor, &last_name,
			    &first_name, &term_start, &term_end)) != 0)
				break;
		}
	}
	if (!RET_OK(ret))
		fprintf(stderr, "Error %d for year %" PRIu16 "\n", ret, year);

	ret = cursor->close(cursor);
	return (ret);
}

/*
 * Remove some items from the primary table.
 */
static int
remove_items(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	struct president_data p;
	int i, ret;

	/*
	 * Removing items from the primary table will call the extractor
	 * for the index and allow our custom extractor code to handle
	 * each custom key.
	 */
	ret = session->open_cursor(
	    session, "table:presidents", NULL, NULL, &cursor);
	/*
	 * Just remove the first few items.
	 */
	for (i = 0; example_data[i].last_name != NULL && i < 2; i++) {
		p = example_data[i];
		cursor->set_key(cursor, p.id);
		ret = cursor->remove(cursor);
	}
	return (ret);
}

/*
 * Set up the table and index of the data.
 */
static int
setup_table(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	struct president_data p;
	int i, ret;

	/* Create the primary table. It has a key of the unique ID. */
	ret = session->create(session, "table:presidents",
	    "key_format=I,value_format=SSHH,"
	    "columns=(ID,last_name,first_name,term_begin,term_end)");

	/*
	 * Create the index that is generated with an extractor. The index
	 * will generate an entry in the index for each year a president
	 * was in office.
	 */
	ret = session->create(session, "index:presidents:term",
	    "key_format=H,columns=(term),extractor=my_extractor");

	ret = session->open_cursor(
	    session, "table:presidents", NULL, NULL, &cursor);
	for (i = 0; example_data[i].last_name != NULL; i++) {
		p = example_data[i];
		cursor->set_key(cursor, p.id);
		cursor->set_value(cursor,
		    p.last_name, p.first_name, p.term_start, p.term_end);
		fprintf(stderr,
		    "SETUP: table insert %" PRIu16 "-%" PRIu16 ": %s %s\n",
		    p.term_start, p.term_end,
		    p.first_name, p.last_name);
		ret = cursor->insert(cursor);
	}
	return (ret);
}

int
main(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
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
	ret = conn->open_session(conn, NULL, NULL, &session);

	ret = setup_table(session);
	ret = read_index(session);
	ret = remove_items(session);

	ret = conn->close(conn, NULL);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
