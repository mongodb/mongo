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
 * ex_async.java
 * 	demonstrates how to use the asynchronous API.
 */
package com.wiredtiger.examples;
import com.wiredtiger.db.*;
import java.io.*;
import java.util.*;

/*! [async example callback implementation] */
class AsyncKeys implements AsyncCallback {

    public int numKeys = 0;

    public AsyncKeys() {}

    public void notifyError(String desc) {
        System.err.println("ERROR: notify: " + desc);
    }

    public int notify(AsyncOp op, int opReturn, int flags) {
        /*
         * Note: we are careful not to throw any errors here.  Any
         * exceptions would be swallowed by a native worker thread.
         */
        int ret = 0;
        try {
            /*! [async get type] */
            /* Retrieve the operation's type. */
            AsyncOpType optype = op.getType();
            /*! [async get type] */
            /*! [async get identifier] */
            /* Retrieve the operation's 64-bit identifier. */
            long id = op.getId();
            /*! [async get identifier] */

            /* If doing a search, retrieve the key/value pair. */
            if (optype == AsyncOpType.WT_AOP_SEARCH) {
                /*! [async get the operation's string key] */
                String key = op.getKeyString();
                /*! [async get the operation's string key] */
                /*! [async get the operation's string value] */
                String value = op.getValueString();
                /*! [async get the operation's string value] */
                synchronized (this) {
                    numKeys += 1;
                }
                System.out.println("Id " + id + " got record: " + key +
                                   " : " + value);
            }
        }
        catch (Exception e) {
            System.err.println("ERROR: exception in notify: " + e.toString() +
                               ", opreturn=" + opReturn);
            ret = 1;
        }
        return (ret);
    }
}
/*! [async example callback implementation] */

public class ex_async {

    public static String home;

    public static final int MAX_KEYS = 15;

    public static AsyncOp tryAsyncNewOp(Connection conn, String uri,
        String config, AsyncCallback cb) throws WiredTigerException
    {
        WiredTigerException savedwte = null;

        for (int tries = 0; tries < 10; tries++)
            try {
                return conn.async_new_op(uri, config, cb);
            }
            catch (WiredTigerException wte) {
                /*
                 * If we used up all the handles, pause and retry to
                 * give the workers a chance to catch up.
                 */
                System.err.println(
                    "asynchronous operation handle not available: " + wte);
                savedwte = wte;
                try {
                    Thread.sleep(1);
                } catch (InterruptedException ie) {
                    /* not a big problem, continue to retry */
                }
            }

        throw savedwte;
    }
    
    public static int
    asyncExample()
        throws WiredTigerException
    {
        AsyncOp op;
        Connection conn;
        Session session;
        int i, ret;
        String k[] = new String[MAX_KEYS];
        String v[] = new String[MAX_KEYS];

        /*! [async example callback implementation part 2] */
        AsyncKeys asynciface = new AsyncKeys();
        /*! [async example callback implementation part 2] */

        /*! [async example connection] */
        conn = wiredtiger.open(home, "create,cache_size=100MB," +
            "async=(enabled=true,ops_max=20,threads=2)");
        /*! [async example connection] */

        /*! [async example table create] */
        session = conn.open_session(null);
        ret = session.create("table:async", "key_format=S,value_format=S");
        /*! [async example table create] */

        /* Insert a set of keys asynchronously. */
        for (i = 0; i < MAX_KEYS; i++) {
            /*! [async handle allocation] */
            op = tryAsyncNewOp(conn, "table:async", null, asynciface);
            /*! [async handle allocation] */

            /*! [async insert] */
            /*
             * Set the operation's string key and value, and then do
             * an asynchronous insert.
             */
            /*! [async set the operation's string key] */
            k[i] = "key" + i;
            op.putKeyString(k[i]);
            /*! [async set the operation's string key] */

            /*! [async set the operation's string value] */
            v[i] = "value" + i;
            op.putValueString(v[i]);
            /*! [async set the operation's string value] */

            ret = op.insert();
            /*! [async insert] */
        }

        /*! [async flush] */
        /* Wait for all outstanding operations to complete. */
        ret = conn.async_flush();
        /*! [async flush] */

        /*! [async compaction] */
        /*
         * Compact a table asynchronously, limiting the run-time to 5 minutes.
         */
        op = tryAsyncNewOp(conn, "table:async", "timeout=300", asynciface);
        ret = op.compact();
        /*! [async compaction] */

        /* Search for the keys we just inserted, asynchronously. */
        for (i = 0; i < MAX_KEYS; i++) {
            op = tryAsyncNewOp(conn, "table:async", null, asynciface);
            /*! [async search] */
            /*
             * Set the operation's string key and value, and then do
             * an asynchronous search.
             */
            k[i] = "key" + i;
            op.putKeyString(k[i]);
            ret = op.search();
            /*! [async search] */
        }

        /*
         * Connection close automatically does an async_flush so it will wait
         * for all queued search operations to complete.
         */
        ret = conn.close(null);

        System.out.println("Searched for " + asynciface.numKeys + " keys");

        return (ret);
    }
    
    public static void
    main(String[] argv)
    {
        try {
            System.exit(asyncExample());
        }
        catch (WiredTigerException wte) {
            System.err.println("Exception: " + wte);
            wte.printStackTrace();
            System.exit(1);
        }
    }
}
