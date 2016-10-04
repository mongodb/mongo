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

import org.junit.Test;
import org.junit.Assert;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

public class ConfigTest {
    Connection conn;
    Session session;

    public static final String uri = "table:test_config06";
    public static final String key = "keyABCDEFGHIJKLMNOPQRSTUVWXYZ";
    public static final String value = "valueABCDEFGHIJKLMNOPQRSTUVWXYZ";

    public void session_config(String config) {
        Exception e = null;

        try {
            session.create(uri, config);
        }
        catch (WiredTigerException wte) {
            e = wte;
        }

        Assert.assertTrue(e != null &&
                          e.toString().indexOf("Invalid argument") >= 0);
    }

    // Edge cases for key/value formats.
    @Test
    public void test_session_config()
    throws WiredTigerException {
        setup();
        System.err.println("\n-- expect error output --");
        session_config("key_format=A,value_format=S");
        session_config("key_format=S,value_format=A");
        session_config("key_format=0s,value_format=s");
        session_config("key_format=s,value_format=0s");
        session_config("key_format=0t,value_format=4t");
        session_config("key_format=4t,value_format=0t");
        System.err.println("-- end expected error output --");
        teardown();
    }

    // Smoke-test the string formats with length specifiers; both formats should
    // ignore trailing bytes, verify that.
    public void format_string(String fmt, int len)
    throws WiredTigerException {
        setup();
        session.create(uri, "key_format=" + len + fmt +
                            ",value_format=" + len + fmt);
        Cursor cursor = session.open_cursor(uri, null, null);
        cursor.putKeyString(key);
        cursor.putValueString(value);
        cursor.insert();
        cursor.putKeyString(key.substring(0,len));
        assertEquals(0, cursor.search());
        assertEquals(value.substring(0,len), cursor.getValueString());
        cursor.close();
        session.drop(uri, null);
        teardown();
    }

    @Test
    public void test_format_string_S_1()
    throws WiredTigerException {
        format_string("S", 1);
    }
    @Test
    public void test_format_string_S_4()
    throws WiredTigerException {
        format_string("S", 4);
    }
    @Test
    public void test_format_string_S_10()
    throws WiredTigerException {
        format_string("S", 10);
    }
    @Test
    public void test_format_string_s_1()
    throws WiredTigerException {
        format_string("s", 1);
    }
    @Test
    public void test_format_string_s_4()
    throws WiredTigerException {
        format_string("s", 4);
    }
    @Test
    public void test_format_string_s_10()
    throws WiredTigerException {
        format_string("s", 10);
    }

    @Test
    public void test_format_string_S_default()
    throws WiredTigerException {
        setup();
        session.create(uri, "key_format=S,value_format=S");
        Cursor cursor = session.open_cursor(uri, null, null);
        cursor.putKeyString(key);
        cursor.putValueString(value);
        cursor.insert();
        cursor.putKeyString(key);
        assertEquals(0, cursor.search());
        assertEquals(value, cursor.getValueString());
        cursor.close();
        session.drop(uri, null);
        teardown();
    }

    @Test
    public void test_format_string_s_default()
        throws WiredTigerException {
        setup();
        session.create(uri, "key_format=s,value_format=s");
        Cursor cursor = session.open_cursor(uri, null, null);
        cursor.putKeyString(key);
        cursor.putValueString(value);
        cursor.insert();
        cursor.putKeyString(key.substring(0,1));
        assertEquals(0, cursor.search());
        assertEquals(value.substring(0,1), cursor.getValueString());
        cursor.close();
        session.drop(uri, null);
        teardown();
    }

    public static void main(String[] args) {
        ConfigTest tester = new ConfigTest();
        try {
            tester.test_session_config();
            tester.test_format_string_S_1();
            tester.test_format_string_S_4();
            tester.test_format_string_S_10();
            tester.test_format_string_s_1();
            tester.test_format_string_s_4();
            tester.test_format_string_s_10();
            tester.test_format_string_S_default();
            tester.test_format_string_s_default();
        } catch (WiredTigerException wte) {
            System.err.println("WiredTigerException: " + wte);
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
