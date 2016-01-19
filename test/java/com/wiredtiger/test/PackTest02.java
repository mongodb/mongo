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

import java.util.Arrays;
import com.wiredtiger.db.Connection;
import com.wiredtiger.db.Cursor;
import com.wiredtiger.db.Session;
import com.wiredtiger.db.WiredTigerException;
import com.wiredtiger.db.WiredTigerPackingException;
import com.wiredtiger.db.wiredtiger;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.Assert;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

public class PackTest02 {

    public static final String TEST_NAME = "PackTest02";

    private Connection connection;
    private Session session;

    // Compare two byte arrays, assert if different
    void compareByteArrays(byte[] a1, byte[] a2) {
        //printByteArray(a1, a1.length);
        //printByteArray(a2, a2.length);
        if (a1.length != a2.length) {
            System.err.println("Length differ");
        }
        assertEquals(a1.length, a2.length);
        for (int i = 0; i < a1.length; i++) {
            if (a1[i] != a2[i])
                System.err.println("DIFFER at " + i);
            assertEquals(a1[i], a2[i]);
        }
    }

    // Add either a set of keys or a set of values to the
    // cursor at the current position.
    boolean addKeyValues(Cursor cursor, Object[] args, boolean iskey) {
        for (Object arg : args) {
            if (arg instanceof Integer) {
                if (iskey)
                    cursor.putKeyInt((Integer)arg);
                else
                    cursor.putValueInt((Integer)arg);
            }
            else if (arg instanceof String) {
                if (iskey)
                    cursor.putKeyString((String)arg);
                else
                    cursor.putValueString((String)arg);
            }
            else if (arg instanceof byte[]) {
                if (iskey)
                    cursor.putKeyByteArray((byte[])arg);
                else
                    cursor.putValueByteArray((byte[])arg);
            }
            else
                throw new IllegalArgumentException("unknown type");
        }
        return true;
    }
    
    // Check that either a set of keys or a set of values match
    // the given expected values.  Assert when different.
    boolean checkKeyValues(Cursor cursor, Object[] args, boolean iskey) {
        for (Object arg : args) {
            if (arg instanceof Integer) {
                if (iskey)
                    assertEquals(arg, cursor.getKeyInt());
                else
                    assertEquals(arg, cursor.getValueInt());
            }
            else if (arg instanceof String) {
                if (iskey)
                    assertEquals(arg, cursor.getKeyString());
                else
                    assertEquals(arg, cursor.getValueString());
            }
            else if (arg instanceof byte[]) {
                if (iskey)
                    compareByteArrays((byte[])arg, cursor.getKeyByteArray());
                else
                    compareByteArrays((byte[])arg, cursor.getValueByteArray());
            }
            else
                throw new IllegalArgumentException("unknown type");
        }
        return true;
    }

    // Helper function to make an array out of variable args
    Object[] makeArray(Object... args)
    {
        return args;
    }

    // Use checkCompare for the common case that what
    // we store is what we expect.
    boolean check(String fmt, Object... args)
    {
        return checkCompare(fmt, args, args);
    }

    // Create a table with 'fmt' as the value, and also a
    // reverse index.  Store the given arguments as values,
    // make sure we can retrieve them from the main table
    // and the index.  We compare the result using compareArgs,
    // these may be different from what we store due to padding.
    boolean checkCompare(String fmt, Object[] storeArgs, Object[] compareArgs)
        throws WiredTigerException {
        String uri = "table:" + TEST_NAME + "-" + fmt;
        String idx_uri = "index:" + TEST_NAME + "-" + fmt + ":inverse";
        int nargs = storeArgs.length;
        String colnames = "";
        for (int i = 0; i < nargs; i++) {
            if (i > 0)
                colnames += ",";
            colnames += "v" + i;
        }
        session.create(uri, "columns=(k," + colnames + ")," +
                       "key_format=i,value_format=" + fmt);
        session.create(idx_uri, "columns=(" + colnames + ")");
        Cursor forw = session.open_cursor(uri, null, null);
        Cursor forw_idx = session.open_cursor(idx_uri + "(k)", null, null);

        forw.putKeyInt(1234);
        if (!addKeyValues(forw, storeArgs, false))
            return false;
        forw.insert();

        forw.putKeyInt(1234);
        assertEquals(0, forw.search());
        if (!checkKeyValues(forw, compareArgs, false))
            return false;

        if (!addKeyValues(forw_idx, storeArgs, true))
            return false;
        assertEquals(0, forw_idx.search());

        Integer expected[] = { 1234 };

        if (!checkKeyValues(forw_idx, expected, false))
            return false;

        forw.close();
        forw_idx.close();
        session.drop(idx_uri, null);
        session.drop(uri, null);
        return true;
    }

    // A debug helper method
    private void printByteArray(byte[] bytes, int len) {
        for (int i = 0; i < len; i++) {
            System.out.println(String.format(
                                   "\t%8s", Integer.toBinaryString(
                                       bytes[i] & 0xff)).replace(' ', '0'));
        }
    }

    private void setup() {
        connection = wiredtiger.open("WT_HOME", "create");
        session = connection.open_session(null);
    }

    private void teardown() {
        session.close(null);
        connection.close(null);
    }

    @Test
    public void packTest()
    throws WiredTigerPackingException {

        // Do a test of packing, based on test suite's test_pack.py

        String a10 = "aaaaaaaaaa";
        String a42 = a10 + a10 + a10 + a10 + "aa";
        byte[] b10 =
            { 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42 };
        byte[] b20 =
            { 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42,
              0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42 };
        byte[] b1 = { 0x4 };
        byte[] b3 = { 0x4, 0x0, 0x0 };

        setup();
        check("iii", 0, 101, -99);
        check("3i", 0, 101, -99);
        check("iS", 42, "forty two");
        check("S", "abc");
        check("9S", "aaaaaaaaa");
        check("9sS", "forty two", "spam egg");
        check("42s", a42);
        check("42sS", a42, "something");
        check("S42s", "something", a42);
        // nul terminated string with padding
        check("10sS", "aaaaa\0\0\0\0\0", "something");
        check("S10s", "something", "aaaaa\0\0\0\0\0");
        check("u", b20);
        check("uu", b10, b10);
        // input 1 byte for a 3 byte format, extra null bytes will be stored
        checkCompare("3u", makeArray(b1), makeArray(b3));
        checkCompare("3uu", makeArray(b1, b10), makeArray(b3, b10));
        checkCompare("u3u", makeArray(b10, b1), makeArray(b10, b3));

        check("s", "4");
        check("1s", "4");
        check("2s", "42");
        teardown();
    }

    public static void main(String[] args) {
        PackTest02 tester = new PackTest02();
        try {
            tester.packTest();
        } catch (WiredTigerPackingException wtpe) {
            System.err.println("Packing exception: " + wtpe);
        }
    }
}
