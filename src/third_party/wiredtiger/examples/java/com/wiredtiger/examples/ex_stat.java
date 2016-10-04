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
 * ex_stat.java
 *	This is an example demonstrating how to query database statistics.
 */
package com.wiredtiger.examples;
import com.wiredtiger.db.*;
import java.io.*;
import java.util.*;

public class ex_stat {

    public static String home;

    /*! [statistics display function] */
    int
    print_cursor(Cursor cursor)
        throws WiredTigerException
    {
        String desc, pvalue;
        long value;
        int ret;

        while ((ret = cursor.next()) == 0) {
            desc = cursor.getValueString();
            pvalue = cursor.getValueString();
            value = cursor.getValueLong();
            if (value != 0)
                System.out.println(desc + "=" + pvalue);
        }

        return (ret == wiredtiger.WT_NOTFOUND ? 0 : ret);
    }
    /*! [statistics display function] */

    int 
    print_database_stats(Session session)
        throws WiredTigerException
    {
        Cursor cursor;
        int ret;

        /*! [statistics database function] */
        cursor = session.open_cursor("statistics:", null, null);

        ret = print_cursor(cursor);
        ret = cursor.close();
        /*! [statistics database function] */

        return (ret);
    }

    int 
    print_file_stats(Session session)
        throws WiredTigerException
    {
        Cursor cursor;
        int ret;

        /*! [statistics table function] */
        cursor = session.open_cursor("statistics:table:access", null, null);
        ret = print_cursor(cursor);
        ret = cursor.close();
        /*! [statistics table function] */

        return (ret);
    }

    int 
    print_join_cursor_stats(Session session)
        throws WiredTigerException
    {
	Cursor idx_cursor, join_cursor, stat_cursor;
	int ret;

	ret = session.create("index:access:idx", "columns=(v)");
	idx_cursor = session.open_cursor("index:access:idx", null, null);
	ret = idx_cursor.next();
	join_cursor = session.open_cursor("join:table:access", null, null);
	ret = session.join(join_cursor, idx_cursor, "compare=gt");
	ret = join_cursor.next();

	/*! [statistics join cursor function] */
	stat_cursor = session.open_cursor("statistics:join", join_cursor, null);

	ret = print_cursor(stat_cursor);
	ret = stat_cursor.close();
	/*! [statistics join cursor function] */

	ret = join_cursor.close();
	ret = idx_cursor.close();

	return (ret);
    }

    int
    print_overflow_pages(Session session)
        throws WiredTigerException
    {
        /*! [statistics retrieve by key] */
        Cursor cursor;
        String desc, pvalue;
        long value;
        int ret;

        cursor = session.open_cursor("statistics:table:access", null, null);

        cursor.putKeyInt(wiredtiger.WT_STAT_DSRC_BTREE_OVERFLOW);
        ret = cursor.search();
        desc = cursor.getValueString();
        pvalue = cursor.getValueString();
        value = cursor.getValueLong();
        System.out.println(desc + "=" + pvalue);

        ret = cursor.close();
        /*! [statistics retrieve by key] */

        return (ret);
    }

    /*! [statistics calculation helper function] */
    long
    get_stat(Cursor cursor, int stat_field)
        throws WiredTigerException
    {
        long value;
        int ret;

        cursor.putKeyInt(stat_field);
        if ((ret = cursor.search()) != 0) {
            System.err.println("stat_field: " + stat_field + " not found");
            value = 0;
        }
        else {
            String desc = cursor.getValueString();
            String pvalue = cursor.getValueString();
            value = cursor.getValueLong();
        }
        return (value);
    }
    /*! [statistics calculation helper function] */

    int
    print_derived_stats(Session session)
        throws WiredTigerException
    {
        Cursor cursor;
        int ret;

        /*! [statistics calculate open table stats] */
        cursor = session.open_cursor("statistics:table:access", null, null);
        /*! [statistics calculate open table stats] */

            {
                /*! [statistics calculate table fragmentation] */
                long ckpt_size = get_stat(cursor,
                    wiredtiger.WT_STAT_DSRC_BLOCK_CHECKPOINT_SIZE);
                long file_size = get_stat(cursor,
                    wiredtiger.WT_STAT_DSRC_BLOCK_SIZE);

                System.out.println("File is " +
                    (int)(100 * (file_size - ckpt_size) / file_size) +
                    "% fragmented\n");
                /*! [statistics calculate table fragmentation] */
            }

                {
                    /*! [statistics calculate write amplification] */
                    long app_insert = get_stat(cursor,
                        wiredtiger.WT_STAT_DSRC_CURSOR_INSERT_BYTES);
                    long app_remove = get_stat(cursor,
                        wiredtiger.WT_STAT_DSRC_CURSOR_REMOVE_BYTES);
                    long app_update = get_stat(cursor,
                        wiredtiger.WT_STAT_DSRC_CURSOR_UPDATE_BYTES);

                    long fs_writes = get_stat(cursor,
                        wiredtiger.WT_STAT_DSRC_CACHE_BYTES_WRITE);

                    if (app_insert + app_remove + app_update != 0)
                        System.out.println("Write amplification is " +
                            (double)fs_writes / (app_insert + app_remove + app_update));
                    /*! [statistics calculate write amplification] */
                }

                ret = cursor.close();

                return (ret);
    }

    public int
    statExample()
        throws WiredTigerException
    {
        Connection conn;
        Cursor cursor;
        Session session;
        int ret;

        /*
         * Create a clean test directory for this run of the test program if the
         * environment variable isn't already set (as is done by make check).
         */
        if (System.getenv("WIREDTIGER_HOME") == null) {
            home = "WT_HOME";
            try {
                Process proc = Runtime.getRuntime().exec("/bin/rm -rf " + home);
                BufferedReader br = new BufferedReader(
                    new InputStreamReader(proc.getInputStream()));
                while(br.ready())
                    System.out.println(br.readLine());
                br.close();
                proc.waitFor();
                if (!(new File(home)).mkdir())
                    System.err.println("mkdir: failed");
            } catch (Exception ex) {
                System.err.println("Exception: " + home + ": " + ex);
                System.exit(1);
            }
        } else
            home = null;

        conn = wiredtiger.open(home, "create,statistics=(all)");
        session = conn.open_session(null);

        ret = session.create("table:access",
            "key_format=S,value_format=S,columns=(k,v)");

        cursor = session.open_cursor("table:access", null, null);
        cursor.putKeyString("key");
        cursor.putValueString("value");
        ret = cursor.insert();
        ret = cursor.close();

        ret = session.checkpoint(null);

        ret = print_database_stats(session);

        ret = print_file_stats(session);

        ret = print_join_cursor_stats(session);

        ret = print_overflow_pages(session);

        ret = print_derived_stats(session);

        return (conn.close(null) == 0 ? ret : -1);
    }

    public static void
    main(String[] argv)
    {
        try {
            System.exit((new ex_stat()).statExample());
        }
        catch (WiredTigerException wte) {
            System.err.println("Exception: " + wte);
            wte.printStackTrace();
            System.exit(1);
        }
    }
}
