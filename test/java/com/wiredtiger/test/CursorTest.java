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
        c.addKeyFieldString("bar");
        c.addValueFieldByteArray("foo".getBytes());
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
        c.addKeyFieldString("bar");
        c.addValueFieldByteArray("foo".getBytes());
        c.insert();
        c.addKeyFieldString("bar");
        c.search();
        Assert.assertEquals(c.getKeyFieldString(), "bar");
        Assert.assertEquals(new String(c.getValueFieldByteArray()), "foo");
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
        c.addKeyFieldString("bar");
        c.addValueFieldByteArray("aaaaa".getBytes());
        c.addValueFieldInt(123);
        c.addValueFieldString("eeeee");
        c.addValueFieldByteArray("iiiii".getBytes());
        c.insert();
        c.addKeyFieldString("bar");
        c.search();
        Assert.assertEquals(c.getKeyFieldString(), "bar");
        Assert.assertEquals(new String(c.getValueFieldByteArray()), "aaaaa");
        Assert.assertEquals(c.getValueFieldInt(), 123);
        Assert.assertEquals(c.getValueFieldString(), "eeeee");
        Assert.assertEquals(new String(c.getValueFieldByteArray()), "iiiii");
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
