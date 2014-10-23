/*-
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
import com.wiredtiger.db.WiredTigerPackingException;
import com.wiredtiger.db.WiredTigerRollbackException;
import com.wiredtiger.db.wiredtiger;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;

import org.junit.Test;
import org.junit.Assert;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.FileNotFoundException;
import java.io.PrintStream;

/*
 * A simple class to share state and synchronize actions of
 * two threads in the test
 */
class SharedState {
    public boolean gotRollbackException = false;
    public Exception otherException = null;
}

public class ExceptionTest {
    String uri;
    Connection conn;
    Session s;

    @Test
    public void except01() {
        String keyFormat = "S";
        String valueFormat = "i";
        setup("table:except01", keyFormat, valueFormat);
        boolean caught = false;
        boolean expecting = false; // expecting the exception?
        Cursor c = null;

        try {
            c = s.open_cursor("table:except01", null, null);
            c.putKeyString("bar");
            expecting = true;
            /* expecting int, the following statement should throw */
            c.putValueString("bar");
            expecting = false;
        }
        catch (WiredTigerPackingException wtpe) {
            caught = true;
        }
        finally {
            if (c != null)
                c.close();
        }
        Assert.assertEquals(expecting, true);
        Assert.assertEquals(caught, true);
        teardown();
    }

    @Test
    public void except02() {
        String keyFormat = "S";
        String valueFormat = "i";
        setup("table:except02", keyFormat, valueFormat);
        boolean expecting = false;   // expecting the exception?
        boolean caught = false;
        Cursor c = null;

        System.err.println("\n-- expect error output --");
        try {
            expecting = true;
            /* catch a generic error */
            c = s.open_cursor("nonsense:stuff", null, null);
            expecting = false;
        }
        catch (WiredTigerException wte) {
            Assert.assertEquals(wte.getClass(), WiredTigerException.class);
            caught = true;
        }
        System.err.println("-- end expected error output --");
        Assert.assertEquals(expecting, true);
        Assert.assertEquals(caught, true);
        teardown();
    }

    private void setup(String uriparam, String keyFormat, String valueFormat) {
        uri = uriparam;
        conn = wiredtiger.open("WT_HOME", "create");
        s = conn.open_session(null);
        s.create(uri,
                 "key_format=" + keyFormat + ",value_format=" + valueFormat);
    }

    private void teardown() {
        s.drop(uri, "");
        s.close("");
        conn.close("");
    }

}
