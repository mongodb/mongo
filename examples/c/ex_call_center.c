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

#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))

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
 * In WiredTiger, both tables will use record numbers for IDs, which will be
 * the key.  The C structs for the records are as follows.
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

/* Description of the customer schema. */
static WT_COLUMN_INFO cust_columns[] = {
	{ "id", 0, NULL, NULL },
	{ "name", 1, NULL, NULL },
	{ "address", 1, NULL, NULL },
	{ "phone", 1, NULL, NULL }
};

static const char *cust_phone_cols[] = { "phone" };
static WT_INDEX_INFO cust_indices[] = {
	{ "cust_phone", cust_phone_cols, ARRAY_SIZE(cust_phone_cols) }
};

static WT_SCHEMA cust_schema = {
	"r",		/* Format string for keys (recno). */
	"SSS",		/* Format string for data items: 3 strings */
	cust_columns,	/* Column descriptions. */
	ARRAY_SIZE(cust_columns), /* Number of columns. */
	cust_indices,	/* Index descriptions. */
	ARRAY_SIZE(cust_indices) /* Number of indices. */
};

/* Description of the call record schema. */
static WT_COLUMN_INFO call_columns[] = {
	{ "id", 0, NULL, NULL },
	{ "call_date", 0, NULL, NULL },
	{ "cust_id", 0, NULL, NULL },
	{ "emp_id", 0, NULL, NULL },
	{ "call_type", 0, NULL, NULL },
	{ "notes", 0, NULL, NULL }
};

static const char *call_cust_date_cols[] = { "cust_id", "call_date" };
static WT_INDEX_INFO call_indices[] = {
	{ "call_cust_date",
	    call_cust_date_cols, ARRAY_SIZE(call_cust_date_cols) }
};

static WT_SCHEMA call_schema = {
	"r",		/* Format string for keys (recno). */
	"qrrSS",	/*
			 * Format string for data items:
			 *     i64, 2 recnos, 2 strings
			 */
	call_columns,	/* Column descriptions. */
	ARRAY_SIZE(call_columns), /* Number of columns. */
	call_indices,	/* Index descriptions. */
	ARRAY_SIZE(call_indices) /* Number of indices. */
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
	 * SELECT * FROM Customers WHERE phone=?
	 *
	 * Use the cust_phone index, lookup by phone number to fill the
	 * customer record.  The cursor will have a key format of "S" for a
	 * string because the cust_phone index has a single column ("phone"),
	 * which is of type "S".
	 *
	 * The "with_pkey" option includes the primary key in the cursor's
	 * values, so the value format will be "rSSS".
	 */
	ret = session->open_cursor(session,
	    "index:cust_phone", "with_pkey", &cursor);
	ret = cursor->set_key(cursor, "212-555-1000");
	ret = cursor->search(cursor, &exact);
	if (exact == 0)
		cursor->get_value(cursor, &cust.id,
		    &cust.name, &cust.address, &cust.phone);
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
	 */
	ret = session->open_cursor(session,
	    "index:call_cust_date(cust_id,call_type,notes)", NULL, &cursor);
	ret = cursor->set_key(cursor, cust.id + 1, 0);
	ret = cursor->search(cursor, &exact);
	/* If we found a larger entry, go back one. */
	if (exact > 0)
		ret = cursor->prev(cursor);

	count = 0;
	while ((ret = cursor->get_value(cursor,
	    &call.cust_id, &call.call_type, &call.notes)) == 0 &&
	    call.cust_id == cust.id && count++ < 3) {
		printf("Got call record on date %d: type %s: %s\n",
		    (int)call.call_date, call.call_type, call.notes);

		ret = cursor->prev(cursor);
	}

	ret = conn->close(conn, NULL);

	return (ret);
}
