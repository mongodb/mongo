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
 * ex_thread.java
 *	This is an example demonstrating how to create and access a simple
 *	table from multiple threads.
 */

package com.wiredtiger.examples;
import com.wiredtiger.db.*;
import java.io.*;
import java.util.*;

/*! [thread scan] */
class ScanThread extends Thread {
    private Connection conn;

    public ScanThread(Connection conn) {
        this.conn = conn;
    }

    public void run()
    {
        try {
            int ret;

            Session session = conn.open_session(null);
            Cursor cursor = session.open_cursor("table:access", null, null);

            /* Show all records. */
            while ((ret = cursor.next()) == 0) {
                String key = cursor.getKeyString();
                String value = cursor.getValueString();
                System.out.println("Got record: " + key + " : " + value);
            }
            if (ret != wiredtiger.WT_NOTFOUND)
                System.err.println("Cursor.next: " +
                                   wiredtiger.wiredtiger_strerror(ret));
            cursor.close();
            session.close(null);
        } catch (WiredTigerException wte) {
            System.err.println("Exception " + wte);
        }
    }
}
/*! [thread scan] */

public class ex_thread {

    public static String home;

    public static final int NUM_THREADS = 10;

    /*! [thread main] */
    public static void main(String[] argv)
    {
        try {
            Thread[] threads = new Thread[NUM_THREADS];
            int i, ret;
            Connection conn;

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

            if ((conn = wiredtiger.open(home, "create")) == null) {
                System.err.println("Error connecting to " + home);
                System.exit(1);
            }

            /* Note: further error checking omitted for clarity. */

            Session session = conn.open_session(null);
            ret = session.create("table:access", "key_format=S,value_format=S");
            Cursor cursor = session.open_cursor("table:access", null, "overwrite");
            cursor.putKeyString("key1");
            cursor.putValueString("value1");
            ret = cursor.insert();
            cursor.close();
            ret = session.close(null);

            for (i = 0; i < NUM_THREADS; i++) {
                threads[i] = new ScanThread(conn);
                threads[i].start();
            }

            for (i = 0; i < NUM_THREADS; i++)
                try {
                    threads[i].join();
                    ret = -1;
                }
                catch (InterruptedException ie) {
                }

            ret = conn.close(null);
            System.exit(ret);
        }
        catch (WiredTigerException wte) {
            System.err.println("Exception: " + wte);
            wte.printStackTrace();
            System.exit(1);
        }
    }
    /*! [thread main] */

}
