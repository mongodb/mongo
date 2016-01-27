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

import org.junit.Test;
import org.junit.Assert;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

public class CursorTest {
    Connection conn;
    Session s;

    @Test
    public void cursor01()
    throws WiredTigerPackingException {
        String keyFormat = "S";
        String valueFormat = "u";
        setup(keyFormat, valueFormat);

        Cursor c = s.open_cursor("table:t", null, null);
        c.putKeyString("bar");
        c.putValueByteArray("foo".getBytes());
        c.insert();
        c.close();
        teardown();
    }

    @Test
    public void cursor02()
    throws WiredTigerPackingException {
        String keyFormat = "S";
        String valueFormat = "u";
        setup(keyFormat, valueFormat);

        Cursor c = s.open_cursor("table:t", null, null);
        c.putKeyString("bar");
        c.putValueByteArray("foo".getBytes());
        c.insert();
        c.putKeyString("bar");
        c.search();
        Assert.assertEquals(c.getKeyString(), "bar");
        Assert.assertEquals(new String(c.getValueByteArray()), "foo");
        c.close();
        teardown();
    }

    @Test
    public void cursor03()
    throws WiredTigerPackingException {
        String keyFormat = "S";
        String valueFormat = "uiSu";
        setup(keyFormat, valueFormat);

        Cursor c = s.open_cursor("table:t", null, null);
        c.putKeyString("bar");
        c.putValueByteArray("aaaaa".getBytes()).putValueInt(123);
        c.putValueString("eeeee").putValueByteArray("iiiii".getBytes());

        c.insert();
        c.putKeyString("bar");
        c.search();
        Assert.assertEquals(c.getKeyString(), "bar");
        Assert.assertEquals(new String(c.getValueByteArray()), "aaaaa");
        Assert.assertEquals(c.getValueInt(), 123);
        Assert.assertEquals(c.getValueString(), "eeeee");
        Assert.assertEquals(new String(c.getValueByteArray()), "iiiii");
        c.close();
        teardown();
    }

    private void setup(String keyFormat, String valueFormat) {
        conn = wiredtiger.open("WT_HOME", "create");
        s = conn.open_session(null);
        s.create("table:t",
                 "key_format=" + keyFormat + ",value_format=" + valueFormat);
    }

    private void teardown() {
        s.drop("table:t", "");
        s.close("");
        conn.close("");
    }

}
