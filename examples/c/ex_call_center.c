/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
 * ex_call_center.c
 *	This is an example application that demonstrates how to map a
 *	moderately complex SQL application into WiredTiger.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = "WT_TEST";

/*! [call-center decl] */
/*
 * In SQL, the tables are described as follows:
 *
 * CREATE TABLE Customers(id INTEGER PRIMARY KEY,
 *     name VARCHAR(30), address VARCHAR(50), phone VARCHAR(15))
 * CREATE INDEX CustomersPhone ON Customers(phone)
 *
 * CREATE TABLE Calls(id INTEGER PRIMARY KEY, call_date DATE,
 *     cust_id INTEGER, emp_id INTEGER, call_type VARCHAR(12),
 *     notes VARCHAR(25))
 * CREATE INDEX CallsCustDate ON Calls(cust_id, call_date)
 *
 * In this example, both tables will use record numbers for their IDs, which
 * will be the key.  The C structs for the records are as follows.
 */

/* Customer records. */
typedef struct {
	uint64_t id;
	char *name;
	char *address;
	char *phone;
} CUSTOMER;

/* Call records. */
typedef struct {
	uint64_t id;
	uint64_t call_date;
	uint64_t cust_id;
	uint64_t emp_id;
	char *call_type;
	char *notes;
} CALL;
/*! [call-center decl] */

int main(void)
{
	int count, exact, ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	CUSTOMER cust;
	CALL call;

	ret = wiredtiger_open(home, NULL, "create", &conn);
	if (ret != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
		return (1);
	}
	/* Note: further error checking omitted for clarity. */

	/*! [call-center work] */
	ret = conn->open_session(conn, NULL, NULL, &session);

	/*
	 * Create the customers table, give names and types to the columns.
	 * The columns will be stored in two groups: "main" and "address",
	 * created below.
	 */
	ret = session->create(session, "table:customers",
	    "key_format=S,"
	    "value_format=SSS,"
	    "columns=(id,name,address,phone),"
	    "colgroups=(main,address)");

	/* Create the main column group with value columns except address. */
	ret = session->create(session,
	    "colgroup:customers:main", "columns=(name,phone)");

	/* Create the address column group with just the address. */
	ret = session->create(session,
	    "colgroup:customers:address", "columns=(address)");

	/* Create an index on the customer table by phone number. */
	ret = session->create(session,
	    "index:customers:phone", "columns=(phone)");

	/*
	 * Create the calls table, give names and types to the columns.
	 * All of the columns will be stored together, so no column groups are
	 * declared.
	 */
	ret = session->create(session, "table:calls",
	    "key_format=r,"
	    "value_format=qrrSS,"
	    "columns=(id,call_date,cust_id,emp_id,call_type,notes)");

	/*
	 * Create an index on the calls table with a composite key of cust_id
	 * and call_date.
	 */
	ret = session->create(session, "index:calls:cust_date",
	    "columns=(cust_id,call_date)");

	/* Populate the customers table with some data. */
	ret = session->open_cursor(
	    session, "table:customers", NULL, NULL, &cursor);

	cursor->set_key(cursor, "customer #1");
	cursor->set_value(cursor,
	    "Professor Oak", "LeafGreen Avenue", "123-456-7890");
	ret = cursor->insert(cursor);

	cursor->set_key(cursor, "customer #2");
	cursor->set_value(cursor, "Lorelei", "Sevii Islands", "098-765-4321");
	ret = cursor->insert(cursor);

	ret = cursor->close(cursor);

	/*
	 * First query: a call arrives.  In SQL:
	 *
	 * SELECT id, name FROM Customers WHERE phone=?
	 *
	 * Use the cust_phone index, lookup by phone number to fill the
	 * customer record.  The cursor will have a key format of "S" for a
	 * string because the cust_phone index has a single column ("phone"),
	 * which is of type "S".
	 *
	 * Specify the columns we want: the customer ID and the name.  This
	 * means the cursor's value format will be "rS".
	 */
	ret = session->open_cursor(session,
	    "index:customers:phone(id,name)",
	    NULL, NULL, &cursor);
	cursor->set_key(cursor, "212-555-1000");
	ret = cursor->search(cursor);
	if (ret == 0) {
		ret = cursor->get_value(cursor, &cust.id, &cust.name);
		printf("Got customer record for %s\n", cust.name);
	}
	ret = cursor->close(cursor);

	/*
	 * Next query: get the recent order history.  In SQL:
	 *
	 * SELECT * FROM Calls WHERE cust_id=? ORDER BY call_date DESC LIMIT 3
	 *
	 * Use the call_cust_date index to find the matching calls.  Since it is
	 * is in increasing order by date for a given customer, we want to start
	 * with the last record for the customer and work backwards.
	 *
	 * Specify a subset of columns to be returned.  If these were all
	 * covered by the index, the primary would not be accessed.  Stop after
	 * getting 3 records.
	 */
	ret = session->open_cursor(session,
	    "index:calls:cust_date(cust_id,call_type,notes)",
	    NULL, NULL, &cursor);

	/*
	 * The keys in the index are (cust_id,call_date) -- we want the largest
	 * call date for a given cust_id.  Search for (cust_id+1,0), then work
	 * backwards.
	 */
	cursor->set_key(cursor, cust.id + 1, 0);
	ret = cursor->search_near(cursor, &exact);

	/*
	 * If the table is empty, search_near will return WT_NOTFOUND.
	 * Otherwise the cursor will on a matching key if one exists, or on an
	 * adjacent key.  If the key we find is equal or larger than the search
	 * key, go back one.
	 */
	if (ret == 0 && exact >= 0)
		ret = cursor->prev(cursor);
	if (ret == 0)
		ret = cursor->get_value(cursor,
		    &call.cust_id, &call.call_type, &call.notes);

	count = 0;
	while (ret == 0 && call.cust_id == cust.id) {
		printf("Got call record on date %" PRIu64 ": type %s: %s\n",
		    call.call_date, call.call_type, call.notes);
		if (++count == 3)
			break;

		ret = cursor->prev(cursor);
		ret = cursor->get_value(cursor,
		    &call.cust_id, &call.call_type, &call.notes);
	}
	/*! [call-center work] */

	ret = conn->close(conn, NULL);

	return (ret);
}
