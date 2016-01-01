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
 * ex_access.java
 *    demonstrates how to create and access a simple table.
 */
package com.wiredtiger.examples;
import com.wiredtiger.db.*;

public class ex_access {
    public static void main(String[] args) {
        /*! [access example connection] */
        Connection conn;
        Session s;
        Cursor c;

        try {
            conn = wiredtiger.open("WT_HOME", "create");
            s = conn.open_session(null);
        } catch (WiredTigerException wte) {
            System.err.println("WiredTigerException: " + wte);
            return;
        }
        /*! [access example connection] */
        try {
            /*! [access example table create] */
            s.create("table:t", "key_format=S,value_format=u");
            /*! [access example table create] */
            /*! [access example cursor open] */
            c = s.open_cursor("table:t", null, null);
            /*! [access example cursor open] */
        } catch (WiredTigerException wte) {
            System.err.println("WiredTigerException: " + wte);
            return;
        }
        System.out.println("Key format: " + c.getKeyFormat());
        System.out.println("Value format: " + c.getValueFormat());
        /*! [access example cursor insert] */
        try {
            c.putKeyString("foo");
            c.putValueByteArray("bar".getBytes());
            c.insert();
        } catch (WiredTigerPackingException wtpe) {
            System.err.println("WiredTigerPackingException: " + wtpe);
        } catch (WiredTigerException wte) {
            System.err.println("WiredTigerException: " + wte);
        }
        /*! [access example cursor insert] */
        /*! [access example cursor list] */
        try {
            c.reset();
            while (c.next() == 0) {
                System.out.println("Got: " + c.getKeyString());
            }
        } catch (WiredTigerPackingException wtpe) {
            System.err.println("WiredTigerPackingException: " + wtpe);
        } catch (WiredTigerException wte) {
            System.err.println("WiredTigerException: " + wte);
        }
        /*! [access example cursor list] */

        /*! [access example close] */
        try {
            conn.close(null);
        } catch (WiredTigerException wte) {
            System.err.println("WiredTigerException: " + wte);
        }
        /*! [access example close] */
    }
}
