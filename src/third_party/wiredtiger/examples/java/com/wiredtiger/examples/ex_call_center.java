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
 * ex_call_center.java
 *	This is an example application that demonstrates how to map a
 *	moderately complex SQL application into WiredTiger.
 */

package com.wiredtiger.examples;
import com.wiredtiger.db.*;
import java.io.*;
import java.util.*;

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
class Customer {
    public long id;
    public String name;
    public String address;
    public String phone;
    public Customer(long id, String name, String address, String phone) {
        this.id = id;
        this.name = name;
        this.address = address;
        this.phone = phone;
    }
    public Customer() {}
}

/* Call records. */
class Call {
    public long id;
    public long call_date;
    public long cust_id;
    public long emp_id;
    public String call_type;
    public String notes;
    public Call(long id, long call_date, long cust_id, long emp_id,
                String call_type, String notes) {
        this.id = id;
        this.call_date = call_date;
        this.cust_id = cust_id;
        this.emp_id = emp_id;
        this.call_type = call_type;
        this.notes = notes;
    }
    public Call() {}
}
/*! [call-center decl] */

public class ex_call_center {

    public static String home;

    public static int
    callCenterExample()
	throws WiredTigerException
    {
	Connection conn;
	Cursor cursor;
	Session session;
	int count, ret;
        SearchStatus nearstatus;
        List<Customer> custSample = new ArrayList<Customer>();
        List<Call> callSample = new ArrayList<Call>();

        custSample.add(new Customer(0, "Professor Oak",
            "LeafGreen Avenue", "123-456-7890"));
        custSample.add(new Customer(0, "Lorelei",
            "Sevii Islands", "098-765-4321"));
        callSample.add(new Call(0, 32, 1, 2, "billing", "unavailable"));
        callSample.add(new Call(0, 33, 1, 2, "billing", "available"));
        callSample.add(new Call(0, 34, 1, 2, "reminder", "unavailable"));
        callSample.add(new Call(0, 35, 1, 2, "reminder", "available"));

	/*
	 * Create a clean test directory for this run of the test program if the
	 * environment variable isn't already set (as is done by make check).
	 */
	if (System.getenv("WIREDTIGER_HOME") == null) {
            home = "WT_HOME";
            try {
                Process proc = Runtime.getRuntime().exec("/bin/rm -rf WT_HOME");
                BufferedReader br = new BufferedReader(
                    new InputStreamReader(proc.getInputStream()));
                while(br.ready())
                    System.out.println(br.readLine());
                br.close();
                proc.waitFor();
                new File("WT_HOME").mkdir();
            } catch (Exception ex) {
                System.err.println("Exception: " + ex);
                return (1);
            }
	} else
            home = null;

        try {
            conn = wiredtiger.open(home, "create");
            session = conn.open_session(null);
        } catch (WiredTigerException wte) {
            System.err.println("WiredTigerException: " + wte);
            return(1);
        }
	/* Note: further error checking omitted for clarity. */

	/*! [call-center work] */
	/*
	 * Create the customers table, give names and types to the columns.
	 * The columns will be stored in two groups: "main" and "address",
	 * created below.
	 */
	ret = session.create("table:customers",
	    "key_format=r," +
	    "value_format=SSS," +
	    "columns=(id,name,address,phone)," +
	    "colgroups=(main,address)");

	/* Create the main column group with value columns except address. */
	ret = session.create(
	    "colgroup:customers:main", "columns=(name,phone)");

	/* Create the address column group with just the address. */
	ret = session.create(
	    "colgroup:customers:address", "columns=(address)");

	/* Create an index on the customer table by phone number. */
	ret = session.create(
	    "index:customers:phone", "columns=(phone)");

	/* Populate the customers table with some data. */
	cursor = session.open_cursor("table:customers", null, "append");
	for (Customer cust : custSample) {
            cursor.putValueString(cust.name);
            cursor.putValueString(cust.address);
            cursor.putValueString(cust.phone);
            ret = cursor.insert();
	}
	ret = cursor.close();

	/*
	 * Create the calls table, give names and types to the columns.  All the
	 * columns will be stored together, so no column groups are declared.
	 */
	ret = session.create("table:calls",
	    "key_format=r," +
	    "value_format=qrrSS," +
	    "columns=(id,call_date,cust_id,emp_id,call_type,notes)");

	/*
	 * Create an index on the calls table with a composite key of cust_id
	 * and call_date.
	 */
	ret = session.create("index:calls:cust_date",
	    "columns=(cust_id,call_date)");

	/* Populate the calls table with some data. */
	cursor = session.open_cursor("table:calls", null, "append");
	for (Call call : callSample) {
            cursor.putValueLong(call.call_date);
            cursor.putValueRecord(call.cust_id);
            cursor.putValueRecord(call.emp_id);
            cursor.putValueString(call.call_type);
            cursor.putValueString(call.notes);
            ret = cursor.insert();
	}
	ret = cursor.close();

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
	cursor = session.open_cursor(
	    "index:customers:phone(id,name)", null, null);
	cursor.putKeyString("123-456-7890");
	ret = cursor.search();
	if (ret == 0) {
            Customer cust = new Customer();
            cust.id = cursor.getValueRecord();
            cust.name = cursor.getValueString();
            System.out.println("Read customer record for " + cust.name +
                " (ID " + cust.id + ")");
	}
	ret = cursor.close();

	/*
	 * Next query: get the recent order history.  In SQL:
	 *
	 * SELECT * FROM Calls WHERE cust_id=? ORDER BY call_date DESC LIMIT 3
	 *
	 * Use the call_cust_date index to find the matching calls.  Since it is
	 * is in increasing order by date for a given customer, we want to start
	 * with the last record for the customer and work backwards.
	 *
	 * Specify a subset of columns to be returned.  (Note that if these were
	 * all covered by the index, the primary would not have to be accessed.)
	 * Stop after getting 3 records.
	 */
	cursor = session.open_cursor(
	    "index:calls:cust_date(cust_id,call_type,notes)",
	    null, null);

	/*
	 * The keys in the index are (cust_id,call_date) -- we want the largest
	 * call date for a given cust_id.  Search for (cust_id+1,0), then work
	 * backwards.
	 */
        long custid = 1;
	cursor.putKeyRecord(custid + 1);
	cursor.putKeyLong(0);
	nearstatus = cursor.search_near();

	/*
	 * If the table is empty, search_near will return WT_NOTFOUND, else the
	 * cursor will be positioned on a matching key if one exists, or an
	 * adjacent key if one does not.  If the positioned key is equal to or
	 * larger than the search key, go back one.
	 */
	if (ret == 0 && (nearstatus == SearchStatus.LARGER ||
            nearstatus == SearchStatus.FOUND))
            ret = cursor.prev();
	for (count = 0; ret == 0 && count < 3; ++count) {
            Call call = new Call();
            call.cust_id = cursor.getValueRecord();
            call.call_type = cursor.getValueString();
            call.notes = cursor.getValueString();
            if (call.cust_id != custid)
                break;
            System.out.println("Call record: customer " + call.cust_id +
                               " (" + call.call_type +
                               ": " + call.notes + ")");
            ret = cursor.prev();
	}
	/*! [call-center work] */

	ret = conn.close(null);

	return (ret);
    }

    public static void
    main(String[] argv)
    {
        try {
            System.exit(callCenterExample());
        }
        catch (WiredTigerException wte) {
            System.err.println("Exception: " + wte);
            wte.printStackTrace();
            System.exit(1);
        }
    }
}
