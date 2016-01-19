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
import com.wiredtiger.db.WiredTigerPackingException;
import com.wiredtiger.db.wiredtiger;

import static org.junit.Assert.assertEquals;

import org.junit.After;
import org.junit.Test;
import org.junit.Assert;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

public class AutoCloseTest {

    /*
     * Inner class to hold a session and cursor created in
     * in a thread that may be closed in another thread.
     */
    static class OpenSession extends Thread {
        private final Connection conn;
        private Session sess;
        private Cursor cur;
        private int threadnum;
        public OpenSession(Connection conn, int threadnum) {
            this.conn = conn;
            this.threadnum = threadnum;
        }

        public void run() {
            sess = conn.open_session(null);
            sess.create("table:autoclose", "key_format=S,value_format=S");
            cur = sess.open_cursor("table:autoclose", null, null);
            cur.putKeyString("key" + threadnum);
            cur.putValueString("value" + threadnum);
            cur.insert();
        }

        public void close() {
            cur.close();
            sess.close(null);
        }
    }

    /*
     * Connvalid tells us that we really closed the connection.
     * That allows teardown to reliably clean up so that
     * a single failure in one test does not cascade.
     */
    Connection conn;
    boolean connvalid = false;

    private Session sessionSetup() {
        String keyFormat = "S";
        String valueFormat = "u";

        conn = wiredtiger.open("WT_HOME", "create");
        connvalid = true;
        Session s = conn.open_session(null);
        s.create("table:t",
                 "key_format=" + keyFormat + ",value_format=" + valueFormat);
        return s;
    }

    private Cursor populate(Session s)
    throws WiredTigerPackingException {
        Cursor c = s.open_cursor("table:t", null, null);
        c.putKeyString("bar");
        c.putValueByteArray("foo".getBytes());
        c.insert();
        return c;
    }

    @Test
    public void autoCloseCursor01()
    throws WiredTigerPackingException {
        Session s = sessionSetup();
        Cursor c = populate(s);

        c.close();

        boolean caught = false;
        try {
            // Error: cursor used after close
            c.next();
        }
        catch (NullPointerException iae) {
            assertEquals(iae.toString().contains("cursor is null"), true);
            caught = true;
        }
        assertEquals(caught, true);

        s.close("");
        conn.close("");
        connvalid = false;
    }

    @Test
    public void autoCloseCursor02()
    throws WiredTigerPackingException {
        Session s = sessionSetup();
        Cursor c = populate(s);

        s.close("");

        boolean caught = false;
        try {
            // Error: cursor used after session closed
            c.next();
        }
        catch (NullPointerException iae) {
            assertEquals(iae.toString().contains("cursor is null"), true);
            caught = true;
        }
        assertEquals(caught, true);

        conn.close("");
        connvalid = false;
    }

    @Test
    public void autoCloseCursor03()
    throws WiredTigerPackingException {
        Session s = sessionSetup();
        Cursor c = populate(s);

        conn.close("");
        connvalid = false;

        boolean caught = false;
        try {
            // Error: cursor used after connection close
            c.close();
        }
        catch (NullPointerException iae) {
            assertEquals(iae.toString().contains("cursor is null"), true);
            caught = true;
        }
        assertEquals(caught, true);
    }

    @Test
    public void autoCloseCursor04()
    throws WiredTigerPackingException {
        Session s = sessionSetup();

        // The truncate call allows both of its cursor args
        // to be null, so we don't expect null checking.

        Cursor cbegin = s.open_cursor("table:t", null, null);
        cbegin.putKeyString("bar");
        cbegin.search();
        Cursor cend = s.open_cursor(null, cbegin, null);
        s.truncate(null, cbegin, cend, "");
        cbegin.close();
        cend.close();

        s.truncate("table:t", null, null, null);

        conn.close("");
        connvalid = false;
    }

    @Test
    public void autoCloseCursor05()
    throws WiredTigerPackingException {
        Session s = sessionSetup();
        Cursor c = populate(s);

        // Allowable compare call
        c.putKeyString("bar");
        c.search();
        Cursor c2 = s.open_cursor(null, c, null);
        c.compare(c2);

        boolean caught = false;
        try {
            // Error: cursor arg should not be null
            c.compare(null);
        }
        catch (NullPointerException iae) {
            assertEquals(iae.toString().contains("other is null"), true);
            caught = true;
        }
        assertEquals(caught, true);
        conn.close("");
        connvalid = false;
    }

    @Test
    public void autoCloseSession01()
    throws WiredTigerPackingException {
        Session s = sessionSetup();
        Cursor c = populate(s);

        s.close("");

        boolean caught = false;
        try {
            // Error: session used after close
            s.drop("table:t", "");
        }
        catch (NullPointerException iae) {
            assertEquals(iae.toString().contains("session is null"), true);
            caught = true;
        }
        assertEquals(caught, true);

        conn.close("");
        connvalid = false;
    }

    @Test
    public void autoCloseSession02()
    throws WiredTigerPackingException {
        Session s = sessionSetup();
        Cursor c = populate(s);

        conn.close("");
        connvalid = false;

        boolean caught = false;
        try {
            // Error: session used after connection close
            s.close("");
        }
        catch (NullPointerException iae) {
            assertEquals(iae.toString().contains("session is null"), true);
            caught = true;
        }
        assertEquals(caught, true);
    }

    @Test
    public void autoCloseSession03()
    throws WiredTigerPackingException {
        Session s = sessionSetup();
        Cursor c = populate(s);

        s.close("");

        boolean caught = false;
        try {
            // Error: session used after close, using open call
            s.open_cursor("table:t", null, null);
        }
        catch (NullPointerException iae) {
            // The exception message is different, but still informative.
            assertEquals(iae.toString().contains("self is null"), true);
            caught = true;
        }
        assertEquals(caught, true);

        conn.close("");
        connvalid = false;
    }

    @Test
    public void autoCloseConnection01()
    throws WiredTigerPackingException {
        Session s = sessionSetup();
        Cursor c = populate(s);

        conn.close("");
        connvalid = false;

        boolean caught = false;
        try {
            // Error: connection used after close
            conn.close("");
        }
        catch (NullPointerException iae) {
            assertEquals(iae.toString().contains("connection is null"), true);
            caught = true;
        }
        assertEquals(caught, true);
    }

    public void multithreadedHelper(boolean explicitClose, int nthreads)
    throws WiredTigerPackingException {
        Session s = sessionSetup();
        Cursor c = populate(s);

        OpenSession[] threads = new OpenSession[nthreads];
        for (int i = 0; i < nthreads; i++) {
            threads[i] = new OpenSession(conn, i);
            threads[i].start();
        }

        for (int i = 0; i < nthreads; i++) {
            try {
                threads[i].join();
                if (explicitClose)
                    threads[i].close();
            } catch (InterruptedException e) {
            }
        }

        try {
            conn.close("");
            connvalid = false;
        }
        catch (Exception e) {
            // nothing
        }
        assertEquals(connvalid, false);
    }

    @Test
    public void autoCloseConnection02()
    throws WiredTigerPackingException {
        multithreadedHelper(false, 2);
    }

    @Test
    public void autoCloseConnection03()
    throws WiredTigerPackingException {
        multithreadedHelper(true, 2);
    }

    @Test
    public void autoCloseConnection04()
    throws WiredTigerPackingException {
        multithreadedHelper(false, 10);
    }

    @Test
    public void autoCloseConnection05()
    throws WiredTigerPackingException {
        multithreadedHelper(true, 10);
    }

    @After
    public void teardown() {
        if (connvalid) {
            conn.close("");
        }
    }

}
