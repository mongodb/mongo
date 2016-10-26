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
 */
package com.wiredtiger.test;

import com.wiredtiger.db.Connection;
import com.wiredtiger.db.Cursor;
import com.wiredtiger.db.Session;
import com.wiredtiger.db.WiredTigerException;
import com.wiredtiger.db.wiredtiger;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;

import org.junit.Test;
import org.junit.Assert;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

/*
 * Test multiple threads concurrently inserting and reading.
 * Each insert thread opens and closes for each record, so the
 * test stresses the concurrency of these calls in particular.
 * This is a test case for a problem reported in WT-2788.
 */
class InsertThread extends Thread {
    private Connection conn;
    private int threadId;

    public InsertThread(Connection conn, int threadId) {
        this.conn = conn;
        this.threadId = threadId;
    }

    public void run()
    {
        try {
            int ret;
            for (int i = 0; i < 500; i++) {
                Session session = conn.open_session(null);
                Cursor cursor = session.open_cursor("table:cclose", null,
                                                    "overwrite");
                cursor.putKeyString("key" + threadId + "-" + i);
                cursor.putValueString("value1");
                ret = cursor.insert();
                cursor.close();
                ret = session.close(null);
	    }
        } catch (WiredTigerException wte) {
            System.err.println("Exception " + wte);
        }
    }
}

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
            Cursor cursor = session.open_cursor("table:cclose", null, null);

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

public class ConcurrentCloseTest {
    Connection conn;
    Session session;

    public static String home;

    public static final int NUM_THREADS = 10;

    @Test
    public void test_threads()
    {
        setup();
        try {
            List<Thread> threads = new ArrayList<Thread>();
            int i;

            assertEquals(0, session.create("table:cclose",
                                           "key_format=S,value_format=S"));
            Cursor cursor = session.open_cursor("table:cclose", null,
                                                "overwrite");
            cursor.putKeyString("key1");
            cursor.putValueString("value1");
            assertEquals(0, cursor.insert());
            cursor.close();
            assertEquals(0, session.close(null));

            for (i = 0; i < NUM_THREADS; i++) {
                Thread insertThread = new InsertThread(conn, i);
                Thread scanThread = new ScanThread(conn);
                insertThread.start();
                scanThread.start();
                threads.add(insertThread);
                threads.add(scanThread);
            }
            for (Thread thread : threads)
                try {
                    thread.join();
                }
                catch (InterruptedException ie) {
                    fail();
                }

            assertEquals(0, conn.close(null));
            System.exit(0);
        }
        catch (WiredTigerException wte) {
            System.err.println("Exception: " + wte);
            wte.printStackTrace();
            System.exit(1);
        }
    }

    private void setup() {
        conn = wiredtiger.open("WT_HOME", "create");
        session = conn.open_session(null);
    }

    private void teardown() {
        session.close("");
        conn.close("");
    }


}
