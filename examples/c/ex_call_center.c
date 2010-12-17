/*
 * ex_call_center.c Copyright (c) 2010 WiredTiger
 *
 * This is an example application that demonstrates how to map a moderately
 * complex SQL application into WiredTiger.
 */

#include <stdio.h>
#include <string.h>

#include <inttypes.h>
#include <wiredtiger.h>

const char *home = "WT_TEST";

/*
 * In SQL, the tables are described as follows:
 *
 * CREATE TABLE Customers(id INTEGER PRIMARY KEY,
 *     name VARCHAR(30), address VARCHAR(50), phone VARCHAR(15))
 * CREATE INDEX CustomersPhone ON Cusomters(phone)
 *
 * CREATE TABLE Calls(id INTEGER PRIMARY KEY, call_date DATE,
 *     cust_id INTEGER, emp_id INTEGER, call_type VARCHAR(12), notes VARCHAR(25))
 * CREATE INDEX CallsCustDate ON Calls(cust_id, call_date)
 *
 * In this example, both tables will use record numbers for their IDs, which
 * will be the key.  The C structs for the records are as follows.
 */
typedef struct {
	wiredtiger_recno_t id;
	char *name;
	char *address;
	char *phone;
} CUSTOMER;

typedef struct {
	wiredtiger_recno_t id;
	uint64_t call_date;
	wiredtiger_recno_t cust_id;
	wiredtiger_recno_t emp_id;
	char *call_type;
	char *notes;
} CALL;

/* Store address in a column of its own. */
static WT_SCHEMA_COLUMN_SET cust_column_sets[] = {
	{ "cust_address", "address", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

static WT_SCHEMA_INDEX cust_indices[] = {
	{ "cust_phone", "phone", NULL , NULL },
	{ NULL, NULL, NULL, NULL }
};

/* Description of the customer schema. */
static WT_SCHEMA cust_schema = {
	"r",		/* Format string for keys (recno). */
	"SSS",		/* Format string for data items: 3 strings */
	/* Columns */
	"id,name,address,phone",
	cust_column_sets,
	cust_indices
};

static WT_SCHEMA_INDEX call_indices[] = {
	{ "call_cust_date", "cust_id,call_date", NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

/* Description of the call record schema. */
static WT_SCHEMA call_schema = {
	"r",		/* Format string for keys (recno). */
	"qrrSS",	/*
			 * Format string for data items:
			 *     i64, 2 recnos, 2 strings
			 */
	/* Columns. */
	"id,call_date,cust_id,emp_id,call_type,notes",
	/* Column sets. */
	NULL,
	call_indices
};

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
	/* Note: error checking omitted for clarity. */

	if (conn->is_new(conn)) {
		ret = conn->add_schema(conn, "CUSTOMER", &cust_schema, NULL);
		ret = conn->add_schema(conn, "CALL", &call_schema, NULL);
		ret = session->create_table(session, "customers",
		    "schema=CUSTOMER");
		ret = session->create_table(session, "calls",
		    "schema=CALL");

		/* Omitted: populate the tables with some data. */
	}

	ret = conn->open_session(conn, NULL, NULL, &session);

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
	    "index:cust_phone(id,name)", NULL, &cursor);
	ret = cursor->set_key(cursor, "212-555-1000");
	ret = cursor->search(cursor, &exact);
	if (exact == 0)
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
	    "index:call_cust_date(cust_id,call_type,notes)", NULL, &cursor);
	ret = cursor->set_key(cursor, cust.id + 1, 0);
	ret = cursor->search(cursor, &exact);
	/* If we found a larger entry, go back one. */
	if (exact > 0)
		ret = cursor->prev(cursor);
	ret = cursor->get_value(cursor,
	    &call.cust_id, &call.call_type, &call.notes);

	count = 0;
	while (call.cust_id == cust.id) {
		printf("Got call record on date %d: type %s: %s\n",
		    (int)call.call_date, call.call_type, call.notes);
		if (++count == 3)
			break;

		ret = cursor->prev(cursor);
		ret = cursor->get_value(cursor,
		    &call.cust_id, &call.call_type, &call.notes);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}
