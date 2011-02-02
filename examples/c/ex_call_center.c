/*
 * ex_call_center.c
 * Copyright (c) 2010 WiredTiger, Inc.  All rights reserved.
 *
 * This is an example application that demonstrates how to map a moderately
 * complex SQL application into WiredTiger.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = "WT_TEST";

/*
 * In SQL, the tables are described as follows:
 *
 * CREATE TABLE Customers(id INTEGER PRIMARY KEY,
 *     name VARCHAR(30), address VARCHAR(50), phone VARCHAR(15))
 * CREATE INDEX CustomersPhone ON Customers(phone)
 *
 * CREATE TABLE Calls(id INTEGER PRIMARY KEY, call_date DATE,
 *     cust_id INTEGER, emp_id INTEGER, call_type VARCHAR(12), notes VARCHAR(25))
 * CREATE INDEX CallsCustDate ON Calls(cust_id, call_date)
 *
 * In this example, both tables will use record numbers for their IDs, which
 * will be the key.  The C structs for the records are as follows.
 */

/* Customer records. */
typedef struct {
	wiredtiger_recno_t id;
	char *name;
	char *address;
	char *phone;
} CUSTOMER;

/* Call records. */
typedef struct {
	wiredtiger_recno_t id;
	uint64_t call_date;
	wiredtiger_recno_t cust_id;
	wiredtiger_recno_t emp_id;
	char *call_type;
	char *notes;
} CALL;

int main()
{
	int count, exact, ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	CUSTOMER cust;
	CALL call;

	ret = wiredtiger_open(home, NULL, "create", &conn);
	if (ret != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	ret = conn->open_session(conn, NULL, NULL, &session);

	ret = session->create_table(session, "customers",
	    "key_format=r,"
	    "value_format=SSS,"
	    "columns=(id,name,address,phone),"
	    "colgroup.cust_address=(address),"
	    "index.cust_phone=(phone)");

	ret = session->create_table(session, "calls",
	    "key_format=r,"
	    "value_format=qrrSS,"
	    "columns=(id,call_date,cust_id,emp_id,call_type,notes),"
	    "index.calls_cust_date=(cust_id,call_date)");

	/* Omitted: populate the tables with some data. */

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
	    "index:cust_phone(id,name)",
	    NULL, NULL, &cursor);
	cursor->set_key(cursor, "212-555-1000");
	ret = cursor->search(cursor);
	ret = cursor->get_value(cursor, &cust.id, &cust.name);
	printf("Got customer record for %s\n", cust.name);
	ret = cursor->close(cursor, NULL);

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
	    "index:calls_cust_date(cust_id,call_type,notes)",
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
	if (exact >= 0)
		ret = cursor->prev(cursor);
	ret = cursor->get_value(cursor,
	    &call.cust_id, &call.call_type, &call.notes);

	count = 0;
	while (call.cust_id == cust.id) {
		printf("Got call record on date %lu: type %s: %s\n",
		    (unsigned long)call.call_date, call.call_type, call.notes);
		if (++count == 3)
			break;

		ret = cursor->prev(cursor);
		ret = cursor->get_value(cursor,
		    &call.cust_id, &call.call_type, &call.notes);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}
