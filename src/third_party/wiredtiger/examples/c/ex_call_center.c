/*-
 * Public Domain 2014-present MongoDB, Inc.
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
 * ex_call_center.c
 *	This is an example application that demonstrates how to map a
 *	moderately complex SQL application into WiredTiger.
 */
#include <test_util.h>

static const char *home;

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
    const char *name;
    const char *address;
    const char *phone;
} CUSTOMER;

/* Call records. */
typedef struct {
    uint64_t id;
    uint64_t call_date;
    uint64_t cust_id;
    uint64_t emp_id;
    const char *call_type;
    const char *notes;
} CALL;
/*! [call-center decl] */

int
main(int argc, char *argv[])
{
    int count, exact;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    CUSTOMER cust, *custp,
      cust_sample[] = {{0, "Professor Oak", "LeafGreen Avenue", "123-456-7890"},
        {0, "Lorelei", "Sevii Islands", "098-765-4321"}, {0, NULL, NULL, NULL}};
    CALL call, *callp,
      call_sample[] = {{0, 32, 1, 2, "billing", "unavailable"},
        {0, 33, 1, 2, "billing", "available"}, {0, 34, 1, 2, "reminder", "unavailable"},
        {0, 35, 1, 2, "reminder", "available"}, {0, 0, 0, 0, NULL, NULL}};

    home = example_setup(argc, argv);
    error_check(wiredtiger_open(home, NULL, "create", &conn));

    /*! [call-center work] */
    error_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Create the customers table, give names and types to the columns. The columns will be stored
     * in two groups: "main" and "address", created below.
     */
    error_check(session->create(session, "table:customers",
      "key_format=r,value_format=SSS,columns=(id,name,address,phone),colgroups=(main,address)"));

    /* Create the main column group with value columns except address. */
    error_check(session->create(session, "colgroup:customers:main", "columns=(name,phone)"));

    /* Create the address column group with just the address. */
    error_check(session->create(session, "colgroup:customers:address", "columns=(address)"));

    /* Create an index on the customer table by phone number. */
    error_check(session->create(session, "index:customers:phone", "columns=(phone)"));

    /* Populate the customers table with some data. */
    error_check(session->open_cursor(session, "table:customers", NULL, "append", &cursor));
    for (custp = cust_sample; custp->name != NULL; custp++) {
        cursor->set_value(cursor, custp->name, custp->address, custp->phone);
        error_check(cursor->insert(cursor));
    }
    error_check(cursor->close(cursor));

    /*
     * Create the calls table, give names and types to the columns. All the columns will be stored
     * together, so no column groups are declared.
     */
    error_check(session->create(session, "table:calls",
      "key_format=r,value_format=qrrSS,columns=(id,call_date,cust_id,emp_id,call_type,notes)"));

    /*
     * Create an index on the calls table with a composite key of cust_id and call_date.
     */
    error_check(session->create(session, "index:calls:cust_date", "columns=(cust_id,call_date)"));

    /* Populate the calls table with some data. */
    error_check(session->open_cursor(session, "table:calls", NULL, "append", &cursor));
    for (callp = call_sample; callp->call_type != NULL; callp++) {
        cursor->set_value(
          cursor, callp->call_date, callp->cust_id, callp->emp_id, callp->call_type, callp->notes);
        error_check(cursor->insert(cursor));
    }
    error_check(cursor->close(cursor));

    /*
     * First query: a call arrives. In SQL:
     *
     * SELECT id, name FROM Customers WHERE phone=?
     *
     * Use the cust_phone index, lookup by phone number to fill the customer record. The cursor will
     * have a key format of "S" for a string because the cust_phone index has a single column
     * ("phone"), which is of type "S".
     *
     * Specify the columns we want: the customer ID and the name. This means the cursor's value
     * format will be "rS".
     */
    error_check(
      session->open_cursor(session, "index:customers:phone(id,name)", NULL, NULL, &cursor));
    cursor->set_key(cursor, "123-456-7890");
    error_check(cursor->search(cursor));
    error_check(cursor->get_value(cursor, &cust.id, &cust.name));
    printf("Read customer record for %s (ID %" PRIu64 ")\n", cust.name, cust.id);
    error_check(cursor->close(cursor));

    /*
     * Next query: get the recent order history. In SQL:
     *
     * SELECT * FROM Calls WHERE cust_id=? ORDER BY call_date DESC LIMIT 3
     *
     * Use the call_cust_date index to find the matching calls. Since it is in increasing order by
     * date for a given customer, we want to start with the last record for the customer and work
     * backwards.
     *
     * Specify a subset of columns to be returned. (Note that if these were all covered by the
     * index, the primary would not have to be accessed.) Stop after getting 3 records.
     */
    error_check(session->open_cursor(
      session, "index:calls:cust_date(cust_id,call_type,notes)", NULL, NULL, &cursor));

    /*
     * The keys in the index are (cust_id,call_date) -- we want the largest call date for a given
     * cust_id. Search for (cust_id+1,0), then work backwards.
     */
    cust.id = 1;
    cursor->set_key(cursor, cust.id + 1, 0);
    error_check(cursor->search_near(cursor, &exact));

    /*
     * If the table is empty, search_near will return WT_NOTFOUND, else the cursor will be
     * positioned on a matching key if one exists, or an adjacent key if one does not. If the
     * positioned key is equal to or larger than the search key, go back one.
     */
    if (exact >= 0)
        error_check(cursor->prev(cursor));
    for (count = 0; count < 3; ++count) {
        error_check(cursor->get_value(cursor, &call.cust_id, &call.call_type, &call.notes));
        if (call.cust_id != cust.id)
            break;
        printf(
          "Call record: customer %" PRIu64 " (%s: %s)\n", call.cust_id, call.call_type, call.notes);
        error_check(cursor->prev(cursor));
    }
    /*! [call-center work] */

    error_check(conn->close(conn, NULL));

    return (EXIT_SUCCESS);
}
