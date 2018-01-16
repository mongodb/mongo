/*-
 * Public Domain 2014-2018 MongoDB, Inc.
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
import com.wiredtiger.db.SearchStatus;
import com.wiredtiger.db.Session;
import com.wiredtiger.db.WiredTigerPackingException;
import com.wiredtiger.db.WiredTigerException;
import com.wiredtiger.db.wiredtiger;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.Assert;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

/*
 * Test cases for WT-3238.
 *
 * Most WiredTiger methods return int, and our SWIG typemaps for Java add
 * checking that throws exceptions for non-zero returns.  Certain methods
 * (Cursor.compare, Cursor.equals) are declared as returning int in Java,
 * but should not throw exceptions for normal returns (which may be
 * non-zero).
 */
public class CursorTest03 {
    Connection conn;
    Session s;
    static String values[] = { "key0", "key1" };

    @Test
    public void cursor_int_methods()
    throws WiredTigerPackingException {
        setup();

        Cursor c1 = s.open_cursor("table:t", null, null);
        Cursor c2 = s.open_cursor("table:t", null, null);
        for (String s : values) {
            c1.putKeyString(s);
            c1.putValueString(s);
            c1.insert();
        }
        c1.reset();

        // "key1" compared to "key1"
        c1.putKeyString(values[1]);
        Assert.assertEquals(c1.search_near(), SearchStatus.FOUND);
        c2.putKeyString(values[1]);
        Assert.assertEquals(c2.search_near(), SearchStatus.FOUND);
        Assert.assertEquals(c1.compare(c2), 0);
        Assert.assertEquals(c2.compare(c1), 0);
        Assert.assertEquals(c1.compare(c1), 0);
        Assert.assertEquals(c1.equals(c2), 1);
        Assert.assertEquals(c2.equals(c1), 1);
        Assert.assertEquals(c1.equals(c1), 1);

        // "key0" compared to "key1"
        c1.putKeyString(values[0]);
        Assert.assertEquals(c1.search_near(), SearchStatus.FOUND);
        Assert.assertEquals(c1.compare(c2), -1);
        Assert.assertEquals(c2.compare(c1), 1);
        Assert.assertEquals(c1.equals(c2), 0);
        Assert.assertEquals(c2.equals(c1), 0);

        c1.close();
        c2.close();
        teardown();
    }

    public void expectException(Cursor c1, Cursor c2)
    {
        boolean caught = false;
        try {
            c1.compare(c2);
        }
        catch (WiredTigerException wte) {
            caught = true;
        }
        Assert.assertTrue(caught);

        caught = false;
        try {
            c1.equals(c2);
        }
        catch (WiredTigerException wte) {
            caught = true;
        }
        Assert.assertTrue(caught);
    }

    @Test
    public void cursor_int_methods_errors()
    throws WiredTigerPackingException {
        setup();

        Cursor c1 = s.open_cursor("table:t", null, null);
        Cursor c2 = s.open_cursor("table:t", null, null);
        Cursor cx = s.open_cursor("table:t2", null, null);
        for (String s : values) {
            c1.putKeyString(s);
            c1.putValueString(s);
            c1.insert();
            cx.putKeyString(s);
            cx.putValueString(s);
            cx.insert();
        }
        c1.reset();
        cx.reset();

        // With both cursors not set, should be an exception.
        expectException(c1, c2);
        expectException(c1, c2);

        // With any one cursor not set, should be an exception.
        c1.putKeyString(values[1]);
        Assert.assertEquals(c1.search_near(), SearchStatus.FOUND);
        expectException(c1, c2);
        expectException(c1, c2);

        // With two cursors from different tables, should be an exception.
        cx.putKeyString(values[1]);
        Assert.assertEquals(cx.search_near(), SearchStatus.FOUND);
        expectException(c1, cx);
        expectException(c1, cx);

        c1.close();
        c2.close();
        cx.close();
        teardown();
    }

    private void setup() {
        conn = wiredtiger.open("WT_HOME", "create");
        s = conn.open_session(null);
        s.create("table:t", "key_format=S,value_format=S");
        s.create("table:t2", "key_format=S,value_format=S");
    }

    private void teardown() {
        s.drop("table:t", "");
        s.drop("table:t2", "");
        s.close("");
        conn.close("");
    }

}

