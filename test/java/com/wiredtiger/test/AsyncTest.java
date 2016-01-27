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

import com.wiredtiger.db.AsyncCallback;
import com.wiredtiger.db.AsyncOpType;
import com.wiredtiger.db.AsyncOp;
import com.wiredtiger.db.Connection;
import com.wiredtiger.db.Cursor;
import com.wiredtiger.db.Session;
import com.wiredtiger.db.WiredTigerException;
import com.wiredtiger.db.WiredTigerPackingException;
import com.wiredtiger.db.wiredtiger;

import static org.junit.Assert.assertEquals;

import org.junit.After;
import org.junit.Test;
import org.junit.Assert;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

import java.util.HashMap;

abstract class Callback<KeyType, ValueType> implements AsyncCallback {

    /*package*/ int ninsert = 0;
    /*package*/ int nsearch = 0;
    /*package*/ int nerror = 0;

    HashMap<KeyType, ValueType> current = null;
    Object syncobj = null;      // used for locking

    public Callback(HashMap<KeyType, ValueType> current, Object syncobj) {
        this.current = current;
        this.syncobj = syncobj;
    }

    public void notifyError(String desc) {
        System.err.println("ERROR: notify: " + desc);
    }

    public abstract KeyType opToKey(AsyncOp op)
    throws WiredTigerPackingException;
    public abstract ValueType opToValue(AsyncOp op)
    throws WiredTigerPackingException;

    public int notify(AsyncOp op, int opReturn, int flags) {
        /*
         * Note: we are careful not to throw any errors here.  Any
         * exceptions would be swallowed by a native worker thread.
         */
        int ret = 0;
        int ntotal = 0;
        try {
            synchronized (syncobj) {
                ntotal = ninsert + nsearch + nerror;
            }
            KeyType key = opToKey(op);
            ValueType value = opToValue(op);
            AsyncOpType optype = op.getType();

            if (optype == AsyncOpType.WT_AOP_INSERT) {
                synchronized (syncobj) {
                    ninsert += 1;
                }
            }
            else if (optype == AsyncOpType.WT_AOP_SEARCH) {
                synchronized (syncobj) {
                    nsearch += 1;
                }
            }
            else {
                notifyError("unexpected optype");
                ret = 1;
                synchronized (syncobj) {
                    nerror += 1;
                }
            }

            if (!current.get(key).equals(value)) {
                notifyError("unexpected value: wanted \"" + current.get(key) + "\", and got \"" + value + "\"");
                ret = 1;
                synchronized (syncobj) {
                    nerror += 1;
                }
            }
        }
        catch (Exception e) {
            System.err.println("ERROR: exception in notify: " + e.toString() +
                               ", opreturn=" + opReturn +
                               ", ninsert=" + ninsert +
                               ", nsearch=" + nsearch +
                               ", nerror=" + nerror +
                               ", ntotal=" + ntotal);
            ret = 1;
        }
        return (ret);
    }
}

interface TypeAdapter {
    /**
     * puts the key value pair into the async op.
     */
    void putkv(Session s, AsyncOp op, int i)
        throws WiredTigerPackingException;

    /**
     * puts a key value pair into the the reference HashMap.
     */
    void putref(Session s, HashMap map, int i);
}

// SS = String String
class CallbackSS extends Callback<String, String> implements TypeAdapter {
    public CallbackSS(HashMap<String, String> current, Object syncobj) {
        super(current, syncobj);
    }

    public String opToKey(AsyncOp op)
    throws WiredTigerPackingException {
        return op.getKeyString();
    }

    public String opToValue(AsyncOp op)
    throws WiredTigerPackingException {
        return op.getValueString();
    }

    private String genkey(Session s, int i) {
        return "key" + i;
    }

    private String genvalue(Session s, int i) {
        return "value" + i;
    }

    public void putkv(Session s, AsyncOp op, int i)
        throws WiredTigerPackingException {
        op.putKeyString(genkey(s, i));
        op.putValueString(genvalue(s, i));
    }

    public void putref(Session s, HashMap map, int i) {
        current.put(genkey(s, i), genvalue(s, i));
    }
}

// RS = recno String
class CallbackRS extends Callback<Long, String> implements TypeAdapter {
    public CallbackRS(HashMap<Long, String> current, Object syncobj) {
        super(current, syncobj);
    }

    public Long opToKey(AsyncOp op)
    throws WiredTigerPackingException {
        return op.getKeyLong();
    }

    public String opToValue(AsyncOp op)
    throws WiredTigerPackingException {
        return op.getValueString();
    }

    private long genkey(Session s, int i) {
        return i + 1;
    }

    private String genvalue(Session s, int i) {
        return "value" + i;
    }

    public void putkv(Session s, AsyncOp op, int i)
        throws WiredTigerPackingException {
        op.putKeyLong(genkey(s, i));
        op.putValueString(genvalue(s, i));
    }

    public void putref(Session s, HashMap map, int i) {
        current.put(genkey(s, i), genvalue(s, i));
    }
}

// RI = recno 8t
class CallbackRI extends Callback<Long, Integer> implements TypeAdapter {
    public CallbackRI(HashMap<Long, Integer> current, Object syncobj) {
        super(current, syncobj);
    }

    public Long opToKey(AsyncOp op)
    throws WiredTigerPackingException {
        return op.getKeyLong();
    }

    public Integer opToValue(AsyncOp op)
    throws WiredTigerPackingException {
        return op.getValueInt();
    }

    private long genkey(Session s, int i) {
        return i + 1;
    }

    private int genvalue(Session s, int i) {
        return i % 0xff;
    }

    public void putkv(Session s, AsyncOp op, int i)
        throws WiredTigerPackingException {
        op.putKeyLong(genkey(s, i));
        op.putValueInt(genvalue(s, i));
    }

    public void putref(Session s, HashMap map, int i) {
        current.put(genkey(s, i), genvalue(s, i));
    }
}


public class AsyncTest {

    /*
     * Connvalid tells us that we really closed the connection.
     * That allows teardown to reliably clean up so that
     * a single failure in one test does not cascade.
     */
    Connection conn;
    boolean connvalid = false;

    public static final int N_ENTRIES = 1000;
    public static final int N_OPS_MAX = N_ENTRIES / 2;
    public static final int N_ASYNC_THREADS = 10;

    public static final int MAX_RETRIES = 10;

    private Session sessionSetup(String name,
                                 String keyFormat, String valueFormat,
                                 int opsMax, int asyncThreads) {
        conn = wiredtiger.open("WT_HOME", "create," +
                               "async=(enabled=true,ops_max=" +
                               opsMax + ",threads=" + asyncThreads + ")");
        connvalid = true;
        Session s = conn.open_session(null);
        s.create("table:" + name,
                 "key_format=" + keyFormat + ",value_format=" + valueFormat);
        return s;
    }

    private void sleepMillis(int n)
    {
        try {
            Thread.sleep(n);
        }
        catch (InterruptedException ie) {
            // ignore
        }
    }

    private void asyncTester(String name,
        String keyFormat, String valueFormat,
        int entries, int opsMax, int asyncThreads, int opsBatch,
        int milliSleep, AsyncCallback callback, TypeAdapter adapter,
        HashMap current)
    throws WiredTigerException {
        Session s = sessionSetup(name, keyFormat, valueFormat,
                                 opsMax, asyncThreads);

        for (int i = 0; i < entries; i++) {
            // adapter call does equivalent of:
            //   current.put(genkey(s, i), genvalue(s, i));
            adapter.putref(s, current, i);
        }

        for (int i = 0; i < entries; i++) {
            for (int retry = 1; retry <= MAX_RETRIES; retry++) {
                try {
                    AsyncOp op = conn.async_new_op("table:" + name, null,
                                                   callback);

                    // adapter call does equivalent of:
                    //   op.putKeyString(genkey(s, i));
                    //   op.putValueString(genvalue(s, i));
                    adapter.putkv(s, op, i);

                    op.insert();
                    break;
                }
                catch (WiredTigerException wte) {
                    if (retry == MAX_RETRIES)
                        throw wte;
                    sleepMillis(1 << retry);
                }
            }

            // Introduce a little delay time,
            // otherwise the workers will not get a chance
            // to run before we fill them up.
            // In a real application, we might catch
            // 'Cannot allocate memory' exceptions from
            // the async_new_op call, and use that as
            // an indicator to throttle back.
            if ((i + 1) % opsBatch == 0)
                sleepMillis(milliSleep);
        }

        // Wait for all outstanding async ops to finish.
        conn.async_flush();

        for (int i = 0; i < entries; i++) {
            for (int retry = 1; retry <= MAX_RETRIES; retry++) {
                try {
                    AsyncOp op = conn.async_new_op("table:" + name, null,
                                                   callback);

                    // adapter call does equivalent of:
                    //   op.putKeyString(genkey(s, i));
                    //   op.putValueString(genvalue(s, i));
                    adapter.putkv(s, op, i);

                    op.search();
                    break;
                }
                catch (WiredTigerException wte) {
                    if (retry == MAX_RETRIES)
                        throw wte;
                    sleepMillis(retry);
                }
            }

            if ((i + 1) % opsBatch == 0)
                sleepMillis(milliSleep);
        }

        // Wait for all outstanding async ops to finish.
        conn.async_flush();

        s.close("");

    }

    @Test
    public void async01()
    throws WiredTigerException {
        Object syncobj = new Object();
        HashMap<String, String> current = new HashMap<String, String>();
        CallbackSS callback = new CallbackSS(current, syncobj);

        asyncTester("async01", "S", "S", N_ENTRIES, N_OPS_MAX,
                    N_ASYNC_THREADS, 100, 1, callback, callback, current);

        assertEquals(0, callback.nerror);
        assertEquals(N_ENTRIES, callback.nsearch);
        assertEquals(N_ENTRIES, callback.ninsert);
    }

    @Test
    public void async02()
    throws WiredTigerException {
        Object syncobj = new Object();
        HashMap<String, String> current = new HashMap<String, String>();
        CallbackSS callback = new CallbackSS(current, syncobj);

        asyncTester("async02", "S", "S", 100, 50, 3, 10, 1,
                    callback, callback, current);

        assertEquals(0, callback.nerror);
        assertEquals(100, callback.nsearch);
        assertEquals(100, callback.ninsert);
    }

    @Test
    public void async03()
    throws WiredTigerException {
        Object syncobj = new Object();
        HashMap<String, String> current = new HashMap<String, String>();
        CallbackSS callback = new CallbackSS(current, syncobj);

        asyncTester("async03", "S", "S", 100, 10, 3, 10, 1,
                    callback, callback, current);

        assertEquals(0, callback.nerror);
        assertEquals(100, callback.nsearch);
        assertEquals(100, callback.ninsert);
    }

    @Test
    public void async04()
    throws WiredTigerException {
        Object syncobj = new Object();
        HashMap<Long, String> current = new HashMap<Long, String>();
        CallbackRS callback = new CallbackRS(current, syncobj);

        asyncTester("async04", "q", "S", N_ENTRIES, N_OPS_MAX,
                    N_ASYNC_THREADS, 100, 1, callback, callback, current);

        assertEquals(0, callback.nerror);
        assertEquals(N_ENTRIES, callback.nsearch);
        assertEquals(N_ENTRIES, callback.ninsert);
    }

    @Test
    public void async05()
    throws WiredTigerException {
        Object syncobj = new Object();
        HashMap<Long, Integer> current = new HashMap<Long, Integer>();
        CallbackRI callback = new CallbackRI(current, syncobj);

        asyncTester("async05", "q", "i", N_ENTRIES, N_OPS_MAX,
                    N_ASYNC_THREADS, 100, 1, callback, callback, current);

        assertEquals(0, callback.nerror);
        assertEquals(N_ENTRIES, callback.nsearch);
        assertEquals(N_ENTRIES, callback.ninsert);
    }

    @Test
    public void async06()
    throws WiredTigerException {
        Object syncobj = new Object();
        HashMap<Long, Integer> current = new HashMap<Long, Integer>();
        CallbackRI callback = new CallbackRI(current, syncobj);

        asyncTester("async06", "q", "i", 1000000, 4000,
                    3, 1000000, 0, callback, callback, current);

        assertEquals(0, callback.nerror);
        assertEquals(1000000, callback.nsearch);
        assertEquals(1000000, callback.ninsert);
    }

    @Test
    public void asyncManyConnections01()
    throws WiredTigerException {
        Object syncobj = new Object();
        HashMap<String, String> current = new HashMap<String, String>();

        for (int i = 0; i < 100; i++) {
            CallbackSS callback = new CallbackSS(current, syncobj);
            asyncTester("asyncMany01", "S", "S", 100, 10, 3, 10, 1,
                        callback, callback, current);

            assertEquals(0, callback.nerror);
            assertEquals(100, callback.nsearch);
            assertEquals(100, callback.ninsert);
            conn.close("");
            connvalid = false;
        }
    }

    @Test
    public void asyncManyConnections02()
    throws WiredTigerException {
        Object syncobj = new Object();
        HashMap<Long, Integer> current = new HashMap<Long, Integer>();

        for (int i = 0; i < 10; i++) {
            // These are each long tests, so give some additional feedback
            System.err.print(",");
            CallbackRI callback = new CallbackRI(current, syncobj);
            asyncTester("asyncMany02", "q", "i", 1000000, 4000,
                        3, 1000000, 0, callback, callback, current);

            assertEquals(0, callback.nerror);
            assertEquals(1000000, callback.nsearch);
            assertEquals(1000000, callback.ninsert);
            conn.close("");
            connvalid = false;
        }
    }

    @After
    public void teardown() {
        if (connvalid) {
            conn.close("");
        }
    }

}
