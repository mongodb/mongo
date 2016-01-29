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

public class BackupCursorTest {
    Connection conn;
    Session s;
    public final static int N_TABLES = 10;

    @Test
    public void backupCursor01()
    throws WiredTigerPackingException {
        setup();

        Cursor c = s.open_cursor("backup:", null, null);
        int ntables = 0;

        /*
         * Note: there may be additional files left over from
         * other tests in the suite, ignore them.
         */
        while (c.next() == 0) {
            String backupFile = c.getKeyString();
            if (backupFile.startsWith("backupc"))
                ntables++;
        }

        Assert.assertEquals(N_TABLES, ntables);
        c.close();
        teardown();
    }

    private void setup() {
        conn = wiredtiger.open("WT_HOME", "create");
        s = conn.open_session(null);
        for (int i = 0; i < N_TABLES; i++)
            s.create("table:backupc" + i, "key_format=S,value_format=S");
    }

    private void teardown() {
        for (int i = 0; i < N_TABLES; i++)
            s.drop("table:backupc" + i, "");
        s.close("");
        conn.close("");
    }

}
