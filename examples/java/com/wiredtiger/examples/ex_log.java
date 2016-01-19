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
 * ex_log.java
 * 	demonstrates how to logging and log cursors.
 */
package com.wiredtiger.examples;
import com.wiredtiger.db.*;
import java.io.*;
import java.util.*;

class Lsn {
    int file;
    long offset;
}

public class ex_log {

    public static String home1 = "WT_HOME_LOG_1";
    public static String home2 = "WT_HOME_LOG_2";
    public static String uri = "table:logtest";

    public static final String CONN_CONFIG =
        "create,cache_size=100MB,log=(archive=false,enabled=true)";
    public static final int MAX_KEYS = 10;

    static Session
    setup_copy()
        throws WiredTigerException
    {
        int ret = 0;
        Connection conn;

        conn = wiredtiger.open(home2, CONN_CONFIG);
        Session session = conn.open_session(null);
        session.create(uri, "key_format=U,value_format=U");
        return (session);
    }

    static int
    compare_tables(Session session, Session sess_copy)
        throws WiredTigerException
    {
        int ret;

        Cursor cursor = session.open_cursor(uri, null, null);
        Cursor curs_copy = sess_copy.open_cursor(uri, null, null);

        while ((ret = cursor.next()) == 0) {
            ret = curs_copy.next();
            byte[] key = cursor.getKeyByteArray();
            byte[] value = cursor.getValueByteArray();
            byte[] key_copy = curs_copy.getKeyByteArray();
            byte[] value_copy = curs_copy.getValueByteArray();
            if (!Arrays.equals(key, key_copy) ||
                !Arrays.equals(value, value_copy)) {
                System.err.println(
                    "Mismatched: key " + new String(key) +
                    ", key_copy " + new String(key_copy) +
                    ", value " + new String(value) +
                    ", value_copy " + new String(value_copy));
                return (1);
            }
        }
        if (ret != wiredtiger.WT_NOTFOUND)
            System.err.println("WT_CURSOR.next: " +
                wiredtiger.wiredtiger_strerror(ret));
        ret = cursor.close();

        ret = curs_copy.next();
        if (ret != wiredtiger.WT_NOTFOUND)
            System.err.println("WT_CURSOR.next: " +
                wiredtiger.wiredtiger_strerror(ret));
        ret = curs_copy.close();

        return (ret);
    }

    /*! [log cursor walk] */
    static void
    print_record(Lsn lsn, int opcount,
       int rectype, int optype, long txnid, int fileid,
       byte[] key, byte[] value)
    {
        System.out.print(
            "LSN [" + lsn.file + "][" + lsn.offset + "]." + opcount +
            ": record type " + rectype + " optype " + optype +
            " txnid " + txnid + " fileid " + fileid);
        System.out.println(" key size " + key.length +
            " value size " + value.length);
        if (rectype == wiredtiger.WT_LOGREC_MESSAGE)
            System.out.println("Application Record: " + new String(value));
    }

    /*
     * simple_walk_log --
     *	A simple walk of the log.
     */
    static int
    simple_walk_log(Session session)
        throws WiredTigerException
    {
        Cursor cursor;
        Lsn lsn = new Lsn();
        byte[] logrec_key, logrec_value;
        long txnid;
        int fileid, opcount, optype, rectype;
        int ret;

        /*! [log cursor open] */
        cursor = session.open_cursor("log:", null, null);
        /*! [log cursor open] */

        while ((ret = cursor.next()) == 0) {
            /*! [log cursor get_key] */
            lsn.file = cursor.getKeyInt();
            lsn.offset = cursor.getKeyLong();
            opcount = cursor.getKeyInt();
            /*! [log cursor get_key] */
            /*! [log cursor get_value] */
            txnid = cursor.getValueLong();
            rectype = cursor.getValueInt();
            optype = cursor.getValueInt();
            fileid = cursor.getValueInt();
            logrec_key = cursor.getValueByteArray();
            logrec_value = cursor.getValueByteArray();
            /*! [log cursor get_value] */

            print_record(lsn, opcount,
                rectype, optype, txnid, fileid, logrec_key, logrec_value);
        }
        if (ret == wiredtiger.WT_NOTFOUND)
            ret = 0;
        ret = cursor.close();
        return (ret);
    }
    /*! [log cursor walk] */

    static int
    walk_log(Session session)
        throws WiredTigerException
    {
        Connection wt_conn2;
        Cursor cursor, cursor2;
        Lsn lsn, lsnsave;
        byte[] logrec_key, logrec_value;
        Session session2;
        long txnid;
        int fileid, opcount, optype, rectype;
        int i, ret;
        boolean in_txn, first;

        session2 = setup_copy();
        wt_conn2 = session2.getConnection();
        cursor = session.open_cursor("log:", null, null);
        cursor2 = session2.open_cursor(uri, null, "raw=true");
        i = 0;
        in_txn = false;
        txnid = 0;
        lsn = new Lsn();
        lsnsave = new Lsn();
        while ((ret = cursor.next()) == 0) {
            lsn.file = cursor.getKeyInt();
            lsn.offset = cursor.getKeyLong();
            opcount = cursor.getKeyInt();

            /*
             * Save one of the LSNs we get back to search for it
             * later.  Pick a later one because we want to walk from
             * that LSN to the end (where the multi-step transaction
             * was performed).  Just choose the record that is MAX_KEYS.
             */
            if (++i == MAX_KEYS)
                lsnsave = lsn;
            txnid = cursor.getValueLong();
            rectype = cursor.getValueInt();
            optype = cursor.getValueInt();
            fileid = cursor.getValueInt();
            logrec_key = cursor.getValueByteArray();
            logrec_value = cursor.getValueByteArray();

            print_record(lsn, opcount,
                rectype, optype, txnid, fileid, logrec_key, logrec_value);

            /*
             * If we are in a transaction and this is a new one, end
             * the previous one.
             */
            if (in_txn && opcount == 0) {
                ret = session2.commit_transaction(null);
                in_txn = false;
            }

            /*
             * If the operation is a put, replay it here on the backup
             * connection.  Note, we cheat by looking only for fileid 1
             * in this example.  The metadata is fileid 0.
             */
            if (fileid == 1 && rectype == wiredtiger.WT_LOGREC_COMMIT &&
                optype == wiredtiger.WT_LOGOP_ROW_PUT) {
                if (!in_txn) {
                    ret = session2.begin_transaction(null);
                    in_txn = true;
                }
                cursor2.putKeyByteArray(logrec_key);
                cursor2.putValueByteArray(logrec_value);
                ret = cursor2.insert();
            }
        }
        if (in_txn)
            ret = session2.commit_transaction(null);

        ret = cursor2.close();
        /*
         * Compare the tables after replay.  They should be identical.
         */
        if (compare_tables(session, session2) != 0)
            System.out.println("compare failed");
        ret = session2.close(null);
        ret = wt_conn2.close(null);

        ret = cursor.reset();
        /*! [log cursor set_key] */
        cursor.putKeyInt(lsnsave.file);
        cursor.putKeyLong(lsnsave.offset);
        /*! [log cursor set_key] */
        /*! [log cursor search] */
        ret = cursor.search();
        /*! [log cursor search] */
        System.out.println("Reset to saved...");
        /*
         * Walk all records starting with this key.
         */
        first = true;
        while (ret == 0) {  /*TODO: not quite right*/
            lsn.file = cursor.getKeyInt();
            lsn.offset = cursor.getKeyLong();
            opcount = cursor.getKeyInt();
            if (first) {
                first = false;
                if (lsnsave.file != lsn.file ||
                    lsnsave.offset != lsn.offset) {
                    System.err.println("search returned the wrong LSN");
                    System.exit(1);
                }
            }
            txnid = cursor.getValueLong();
            rectype = cursor.getValueInt();
            optype = cursor.getValueInt();
            fileid = cursor.getValueInt();
            logrec_key = cursor.getValueByteArray();
            logrec_value = cursor.getValueByteArray();

            print_record(lsn, opcount, rectype, optype, txnid,
                fileid, logrec_key, logrec_value);

            ret = cursor.next();
            if (ret != 0)
                break;
        }
        ret = cursor.close();
        return (ret);
    }

    public static int
    logExample()
        throws WiredTigerException
    {
        Connection wt_conn;
        Cursor cursor;
        Session session;
        int i, record_count, ret;

        try {
            String command = "/bin/rm -rf " + home1 + " " + home2;
            Process proc = Runtime.getRuntime().exec(command);
            BufferedReader br = new BufferedReader(
                new InputStreamReader(proc.getInputStream()));
            while(br.ready())
                System.out.println(br.readLine());
            br.close();
            proc.waitFor();
            new File(home1).mkdir();
            new File(home2).mkdir();
        } catch (Exception ex) {
            System.err.println("Exception: " + ex);
            return (1);
        }
        if ((wt_conn = wiredtiger.open(home1, CONN_CONFIG)) == null) {
            System.err.println("Error connecting to " + home1);
            return (1);
        }

        session = wt_conn.open_session(null);
        ret = session.create(uri, "key_format=S,value_format=S");

        cursor = session.open_cursor(uri, null, null);
        /*
         * Perform some operations with individual auto-commit transactions.
         */
        for (record_count = 0, i = 0; i < MAX_KEYS; i++, record_count++) {
            String k = "key" + i;
            String v = "value" + i;
            cursor.putKeyString(k);
            cursor.putValueString(v);
            ret = cursor.insert();
        }
        ret = session.begin_transaction(null);
        /*
         * Perform some operations within a single transaction.
         */
        for (i = MAX_KEYS; i < MAX_KEYS+5; i++, record_count++) {
            String k = "key" + i;
            String v = "value" + i;
            cursor.putKeyString(k);
            cursor.putValueString(v);
            ret = cursor.insert();
        }
        ret = session.commit_transaction(null);
        ret = cursor.close();

        /*! [log cursor printf] */
        ret = session.log_printf("Wrote " + record_count + " records");
        /*! [log cursor printf] */

        session.close(null);
        /*
         * Close and reopen the connection so that the log ends up with
         * a variety of records such as file sync and checkpoint.  We
         * have archiving turned off.
         */
        ret = wt_conn.close(null);
        if ((wt_conn = wiredtiger.open(home1, CONN_CONFIG)) == null) {
            System.err.println("Error connecting to " + home1);
            return (ret);
        }

        session = wt_conn.open_session(null);
        ret = simple_walk_log(session);
        ret = walk_log(session);
        ret = session.close(null);
        ret = wt_conn.close(null);
        return (ret);
    }

    public static void
    main(String[] args)
    {
        try {
            System.exit(logExample());
        }
        catch (WiredTigerException wte) {
            System.err.println("Exception: " + wte);
            wte.printStackTrace();
            System.exit(1);
        }
    }
}
