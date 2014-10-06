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
 * ex_schema.java
 *	This is an example application demonstrating how to create and access
 *	tables using a schema.
 */
package com.wiredtiger.examples;
import com.wiredtiger.db.*;
import java.io.*;
import java.util.*;

public class ex_schema {

    public static String home;

    /*! [schema declaration] */
    /* The class for the data we are storing in a WiredTiger table. */
    static class PopRecord {
        public String country;  // Stored in database as fixed size char[5];
        public short year;
        public long population;
        public PopRecord(String country, short year, long population) {
            this.country = country;
            this.year = year;
            this.population = population;
        }
    }

    static List<PopRecord> popData;

    static {
        popData = new ArrayList<PopRecord>();

        popData.add(new PopRecord("AU",  (short)1900,	  4000000 ));
        popData.add(new PopRecord("AU",  (short)2000,	 19053186 ));
        popData.add(new PopRecord("CAN", (short)1900,	  5500000 ));
        popData.add(new PopRecord("CAN", (short)2000,	 31099561 ));
        popData.add(new PopRecord("UK",  (short)1900,	369000000 ));
        popData.add(new PopRecord("UK",  (short)2000,	 59522468 ));
        popData.add(new PopRecord("USA", (short)1900,	 76212168 ));
        popData.add(new PopRecord("USA", (short)2000,	301279593 ));
    };
    /*! [schema declaration] */

    public static int
    schemaExample()
        throws WiredTigerException
    {
        Connection conn;
        Cursor cursor;
        Session session;
        String country;
        long recno, population;
        short year;
        int ret;

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
                new File("WT_HOME").mkdir();
            } catch (IOException ioe) {
                System.err.println("IOException: WT_HOME: " + ioe);
                return(1);
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

        /*! [Create a table with column groups] */
        /*
         * Create the population table.
         * Keys are record numbers, the format for values is (5-byte string,
         * long, long).
         * See ::wiredtiger_struct_pack for details of the format strings.
         */
        ret = session.create("table:poptable",
            "key_format=r,value_format=5sHQ," +
            "columns=(id,country,year,population),colgroups=(main,population)");

        /*
         * Create two column groups: a primary column group with the country
         * code, year and population (named "main"), and a population column
         * group with the population by itself (named "population").
         */
        ret = session.create("colgroup:poptable:main",
            "columns=(country,year,population)");
        ret = session.create("colgroup:poptable:population",
            "columns=(population)");
        /*! [Create a table with column groups] */

        /*! [Create an index] */
        /* Create an index with a simple key. */
        ret = session.create("index:poptable:country",
            "columns=(country)");
        /*! [Create an index] */

        /*! [Create an index with a composite key] */
        /* Create an index with a composite key (country,year). */
        ret = session.create("index:poptable:country_plus_year",
            "columns=(country,year)");
        /*! [Create an index with a composite key] */

        /*! [Insert and list records] */
        /* Insert the records into the table. */
        cursor = session.open_cursor("table:poptable", null, "append");
        for (PopRecord p : popData) {
            cursor.putValueString(p.country);
            cursor.putValueShort(p.year);
            cursor.putValueLong(p.population);
            ret = cursor.insert();
        }
        ret = cursor.close();

        /* List the records in the table. */
        cursor = session.open_cursor("table:poptable", null, null);
        while ((ret = cursor.next()) == 0) {
            recno = cursor.getKeyLong();
            country = cursor.getValueString();
            year = cursor.getValueShort();
            population = cursor.getValueLong();
            System.out.print("ID " + recno);
            System.out.println(": country " + country + ", year " + year +
                ", population " + population);
        }
        ret = cursor.close();
        /*! [Insert and list records] */

        /*! [List the records in the table using raw mode.] */
        cursor = session.open_cursor("table:poptable", null, "raw");
        while ((ret = cursor.next()) == 0) {
            byte[] key, value;

            key = cursor.getKeyByteArray();
            System.out.println(Arrays.toString(key));
            value = cursor.getValueByteArray();
            System.out.println("raw key: " + Arrays.toString(key) +
                               ", raw value: " + Arrays.toString(value));
        }
        /*! [List the records in the table using raw mode.] */

        /*! [Read population from the primary column group] */
        /*
         * Open a cursor on the main column group, and return the information
         * for a particular country.
         */
        cursor = session.open_cursor("colgroup:poptable:main", null, null);
        cursor.putKeyLong(2);
        if ((ret = cursor.search()) == 0) {
            country = cursor.getValueString();
            year = cursor.getValueShort();
            population = cursor.getValueLong();
            System.out.println("ID 2: country " + country +
                ", year " + year + ", population " + population);
        }
        /*! [Read population from the primary column group] */
        ret = cursor.close();

        /*! [Read population from the standalone column group] */
        /*
         * Open a cursor on the population column group, and return the
         * population of a particular country.
         */
        cursor = session.open_cursor("colgroup:poptable:population", null, null);
        cursor.putKeyLong(2);
        if ((ret = cursor.search()) == 0) {
            population = cursor.getValueLong();
            System.out.println("ID 2: population " + population);
        }
        /*! [Read population from the standalone column group] */
        ret = cursor.close();

        /*! [Search in a simple index] */
        /* Search in a simple index. */
        cursor = session.open_cursor("index:poptable:country", null, null);
        cursor.putKeyString("AU");
        ret = cursor.search();
        country = cursor.getValueString();
        year = cursor.getValueShort();
        population = cursor.getValueLong();
        System.out.println("AU: country " + country + ", year " + year +
                           ", population " + population);
        /*! [Search in a simple index] */
        ret = cursor.close();

        /*! [Search in a composite index] */
        /* Search in a composite index. */
        cursor = session.open_cursor(
            "index:poptable:country_plus_year", null, null);
        cursor.putKeyString("USA");
        cursor.putKeyShort((short)1900);
        ret = cursor.search();
        country = cursor.getValueString();
        year = cursor.getValueShort();
        population = cursor.getValueLong();
        System.out.println("US 1900: country " + country +
           ", year " + year + ", population " + population);
        /*! [Search in a composite index] */
        ret = cursor.close();

        /*! [Return a subset of values from the table] */
        /*
         * Use a projection to return just the table's country and year
         * columns.
         */
        cursor = session.open_cursor("table:poptable(country,year)", null, null);
        while ((ret = cursor.next()) == 0) {
            country = cursor.getValueString();
            year = cursor.getValueShort();
            System.out.println("country " + country + ", year " + year);
        }
        /*! [Return a subset of values from the table] */
        ret = cursor.close();

        /*! [Return a subset of values from the table using raw mode] */
        /*
         * Use a projection to return just the table's country and year
         * columns.
         */
        cursor = session.open_cursor("table:poptable(country,year)", null, null);
        while ((ret = cursor.next()) == 0) {
            country = cursor.getValueString();
            year = cursor.getValueShort();
            System.out.println("country " + country + ", year " + year);
        }
        /*! [Return a subset of values from the table using raw mode] */
        ret = cursor.close();

        /*! [Return the table's record number key using an index] */
        /*
         * Use a projection to return just the table's record number key
         * from an index.
         */
        cursor = session.open_cursor("index:poptable:country_plus_year(id)", null, null);
        while ((ret = cursor.next()) == 0) {
            country = cursor.getKeyString();
            year = cursor.getKeyShort();
            recno = cursor.getValueLong();
            System.out.println("row ID " + recno + ": country " + country +
                ", year " + year);
        }
        /*! [Return the table's record number key using an index] */
        ret = cursor.close();

        /*! [Return a subset of the value columns from an index] */
        /*
         * Use a projection to return just the population column from an
         * index.
         */
        cursor = session.open_cursor(
            "index:poptable:country_plus_year(population)", null, null);
        while ((ret = cursor.next()) == 0) {
            country = cursor.getKeyString();
            year = cursor.getKeyShort();
            population = cursor.getValueLong();
            System.out.println("population " + population +
               ": country " + country + ", year " + year);
        }
        /*! [Return a subset of the value columns from an index] */
        ret = cursor.close();

        /*! [Access only the index] */
        /*
         * Use a projection to avoid accessing any other column groups when
         * using an index: supply an empty list of value columns.
         */
        cursor = session.open_cursor(
            "index:poptable:country_plus_year()", null, null);
        while ((ret = cursor.next()) == 0) {
            country = cursor.getKeyString();
            year = cursor.getKeyShort();
            System.out.println("country " + country + ", year " + year);
        }
        /*! [Access only the index] */
        ret = cursor.close();

        ret = conn.close(null);

        return (ret);
    }

    public static int
    main(String[] argv)
    {
        try {
            return (schemaExample());
        }
        catch (WiredTigerException wte) {
            System.err.println("Exception: " + wte);
            return (-1);
        }
    }
}
