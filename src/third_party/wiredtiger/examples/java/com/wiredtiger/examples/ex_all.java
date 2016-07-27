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
 * ex_all.java
 *    Containing a call to every method in the WiredTiger API.
 *
 *    It doesn't do anything very useful, just demonstrates how to call each
 *    method.  This file is used to populate the API reference with code
 *    fragments.
 */
package com.wiredtiger.examples;
import com.wiredtiger.db.*;
import java.io.*;
import java.nio.*;

/* Note: indentation in non-standard so it will display nicely in doc. */
public class ex_all {

public static String progname = "ex_all";

public static int cursor_ops(Session session)
    throws WiredTigerException
{
    Cursor cursor;
    int ret;

    /*! [Open a cursor] */
    cursor = session.open_cursor("table:mytable", null, null);
    /*! [Open a cursor] */

    /*! [Open a cursor on the metadata] */
    cursor = session.open_cursor("metadata:", null, null);
    /*! [Open a cursor on the metadata] */

    {
    Cursor duplicate;
    String key = "some key";
    /*! [Duplicate a cursor] */
    cursor = session.open_cursor("table:mytable", null, null);
    cursor.putKeyString(key);
    ret = cursor.search();

    /* Duplicate the cursor. */
    duplicate = session.open_cursor(null, cursor, null);
    /*! [Duplicate a cursor] */
    }

    {
    Cursor overwrite_cursor;
    String key = "some key", value = "some value";
    /*! [Reconfigure a cursor] */
    cursor = session.open_cursor("table:mytable", null, null);
    cursor.putKeyString(key);

    /* Reconfigure the cursor to overwrite the record. */
    overwrite_cursor = session.open_cursor(null, cursor, "overwrite");
    ret = cursor.close();

    overwrite_cursor.putValueString(value);
    ret = overwrite_cursor.insert();
    /*! [Reconfigure a cursor] */
    }

    {
    /*! [boolean configuration string example] */
    cursor = session.open_cursor("table:mytable", null, "overwrite");
    cursor = session.open_cursor("table:mytable", null, "overwrite=true");
    cursor = session.open_cursor("table:mytable", null, "overwrite=1");
    /*! [boolean configuration string example] */
    }

    {
    /*! [open a named checkpoint] */
    cursor = session.open_cursor("table:mytable", null, "checkpoint=midnight");
    /*! [open a named checkpoint] */
    }

    {
    /*! [open the default checkpoint] */
    cursor = session.open_cursor("table:mytable", null,
        "checkpoint=WiredTigerCheckpoint");
    /*! [open the default checkpoint] */
    }

    {
    /*! [Get the cursor's string key] */
    String key;	/* Get the cursor's string key. */
    key = cursor.getKeyString();
    /*! [Get the cursor's string key] */
    }

    {
    /*! [Set the cursor's string key] */
    /* Set the cursor's string key. */
    String key = "another key";
    cursor.putKeyString(key);
    /*! [Set the cursor's string key] */
    }

    {
    /*! [Get the cursor's record number key] */
    long recno;		/* Get the cursor's record number key. */
    recno = cursor.getKeyLong();
    /*! [Get the cursor's record number key] */
    }

    {
    /*! [Set the cursor's record number key] */
    long recno = 37;	/* Set the cursor's record number key. */
    cursor.putKeyLong(recno);
    /*! [Set the cursor's record number key] */
    }

    {
    /*! [Get the cursor's composite key] */
    /* Get the cursor's "SiH" format composite key. */
    String first;
    int second;
    short third;

    first = cursor.getKeyString();
    second = cursor.getKeyInt();
    third = cursor.getKeyShort();
    /*! [Get the cursor's composite key] */
    }

    {
    /*! [Set the cursor's composite key] */
    /* Set the cursor's "SiH" format composite key. */
    cursor.putKeyString("first");
    cursor.putKeyInt(5);
    cursor.putKeyShort((short)7);
    /*! [Set the cursor's composite key] */
    }

    {
    /*! [Get the cursor's string value] */
    String value;	/* Get the cursor's string value. */
    value = cursor.getValueString();
    /*! [Get the cursor's string value] */
    }

    {
    /*! [Set the cursor's string value] */
    /* Set the cursor's string value. */
    String value = "another value";
    cursor.putValueString(value);
    /*! [Set the cursor's string value] */
    }

    {
    /*! [Get the cursor's raw value] */
    byte[] value;       /* Get the cursor's raw value. */
    value = cursor.getValueByteArray();
    /*! [Get the cursor's raw value] */
    }

    {
    /*! [Set the cursor's raw value] */
    byte[] value;       /* Set the cursor's raw value. */
    value = "another value".getBytes();
    cursor.putValueByteArray(value);
    /*! [Set the cursor's raw value] */
    }

    /*! [Return the next record] */
    ret = cursor.next();
    /*! [Return the next record] */

    /*! [Return the previous record] */
    ret = cursor.prev();
    /*! [Return the previous record] */

    /*! [Reset the cursor] */
    ret = cursor.reset();
    /*! [Reset the cursor] */

    {
    Cursor other = null;
    /*! [Cursor comparison] */
    int compare;
    compare = cursor.compare(other);
    if (compare == 0) {
        /* Cursors reference the same key */
    } else if (compare < 0) {
        /* Cursor key less than other key */
    } else if (compare > 0) {
        /* Cursor key greater than other key */
    }
    /*! [Cursor comparison] */
    }

    {
    Cursor other = null;
    /*! [Cursor equality] */
    int compare;
    compare = cursor.equals(other);
    if (compare == 0) {
        /* redtiger.iCursors reference the same key */
    } else {
	/* Cursors don't reference the same key */
    }
    /*! [Cursor equality] */
    }

    {
    /*! [Search for an exact match] */
    String key = "some key";
    cursor.putKeyString(key);
    ret = cursor.search();
    /*! [Search for an exact match] */
    }

    cursor_search_near(cursor);

    {
    /*! [Insert a new record or overwrite an existing record] */
    /* Insert a new record or overwrite an existing record. */
    String key = "some key", value = "some value";
    cursor = session.open_cursor("table:mytable", null, null);
    cursor.putKeyString(key);
    cursor.putValueString(value);
    ret = cursor.insert();
    /*! [Insert a new record or overwrite an existing record] */
    }

    {
    /*! [Insert a new record and fail if the record exists] */
    /* Insert a new record and fail if the record exists. */
    String key = "some key", value = "some value";
    cursor = session.open_cursor("table:mytable", null, "overwrite=false");
    cursor.putKeyString(key);
    cursor.putValueString(value);
    ret = cursor.insert();
    /*! [Insert a new record and fail if the record exists] */
    }

    {
    /*! [Insert a new record and assign a record number] */
    /* Insert a new record and assign a record number. */
    long recno;
    String value = "some value";
    cursor = session.open_cursor("table:mytable", null, "append");
    cursor.putValueString(value);
    ret = cursor.insert();
    if (ret == 0)
    recno = cursor.getKeyLong();
    /*! [Insert a new record and assign a record number] */
    }

    {
    /*! [Update an existing record or insert a new record] */
    String key = "some key", value = "some value";
    cursor = session.open_cursor("table:mytable", null, null);
    cursor.putKeyString(key);
    cursor.putValueString(value);
    ret = cursor.update();
    /*! [Update an existing record or insert a new record] */
    }

    {
    /*! [Update an existing record and fail if DNE] */
    String key = "some key", value = "some value";
    cursor = session.open_cursor("table:mytable", null, "overwrite=false");
    cursor.putKeyString(key);
    cursor.putValueString(value);
    ret = cursor.update();
    /*! [Update an existing record and fail if DNE] */
    }

    {
    /*! [Remove a record] */
    String key = "some key";
    cursor = session.open_cursor("table:mytable", null, null);
    cursor.putKeyString(key);
    ret = cursor.remove();
    /*! [Remove a record] */
    }

    {
    /*! [Remove a record and fail if DNE] */
    String key = "some key";
    cursor = session.open_cursor("table:mytable", null, "overwrite=false");
    cursor.putKeyString(key);
    ret = cursor.remove();
    /*! [Remove a record and fail if DNE] */
    }

    {
    /*! [Display an error] */
    try {
        String key = "non-existent key";
        cursor.putKeyString(key);
        if ((ret = cursor.remove()) != 0) {
            System.err.println(
                "cursor.remove: " + wiredtiger.wiredtiger_strerror(ret));
            return (ret);
        }
    } catch (WiredTigerException wte) {  /* Catch severe errors. */
        System.err.println("cursor.remove exception: " + wte);
    }
    /*! [Display an error] */
    }

    {
    /*! [Display an error thread safe] */
    try {
        String key = "non-existent key";
        cursor.putKeyString(key);
        if ((ret = cursor.remove()) != 0) {
            System.err.println(
                "cursor.remove: " + wiredtiger.wiredtiger_strerror(ret));
            return (ret);
        }
    } catch (WiredTigerException wte) {  /* Catch severe errors. */
        System.err.println("cursor.remove exception: " + wte);
    }
    /*! [Display an error thread safe] */
    }

    /*! [Close the cursor] */
    ret = cursor.close();
    /*! [Close the cursor] */

    return (ret);
}

static int
cursor_search_near(Cursor cursor)
    throws WiredTigerException
{
    int ret;
    String key = "some key";
    SearchStatus status;

    /*! [Search for an exact or adjacent match] */
    cursor.putKeyString(key);
    status = cursor.search_near();
    if (status == SearchStatus.FOUND) {
        /* an exact match */
    } else if (status == SearchStatus.SMALLER) {
        /* returned smaller key */
    } else if (status == SearchStatus.LARGER) {
        /* returned larger key */
    } else if (status == SearchStatus.NOTFOUND) {
        /* no match found */
    }
    /*! [Search for an exact or adjacent match] */

    /*! [Forward scan greater than or equal] */
    cursor.putKeyString(key);
    status = cursor.search_near();
    if (status == SearchStatus.FOUND || status == SearchStatus.LARGER) {
        /* include first key returned in the scan */
    }

    while ((ret = cursor.next()) == 0) {
        /* the rest of the scan */
    }
    /*! [Forward scan greater than or equal] */

    /*! [Backward scan less than] */
    cursor.putKeyString(key);
    status = cursor.search_near();
    if (status == SearchStatus.SMALLER) {
        /* include first key returned in the scan */
    }

    while ((ret = cursor.prev()) == 0) {
        /* the rest of the scan */
    }
    /*! [Backward scan less than] */

    return (ret);
}

static int
checkpoint_ops(Session session)
    throws WiredTigerException
{
    int ret;

    /*! [Checkpoint examples] */
    /* Checkpoint the database. */
    ret = session.checkpoint(null);

    /* Checkpoint of the database, creating a named snapshot. */
    ret = session.checkpoint("name=June01");

    /*
     * Checkpoint a list of objects.
     * JSON parsing requires quoting the list of target URIs.
     */
    ret = session.
        checkpoint("target=(\"table:table1\",\"table:table2\")");

    /*
     * Checkpoint a list of objects, creating a named snapshot.
     * JSON parsing requires quoting the list of target URIs.
     */
    ret = session.
        checkpoint("target=(\"table:mytable\"),name=midnight");

    /* Checkpoint the database, discarding all previous snapshots. */
    ret = session.checkpoint("drop=(from=all)");

    /* Checkpoint the database, discarding the "midnight" snapshot. */
    ret = session.checkpoint("drop=(midnight)");

    /*
     * Checkpoint the database, discarding all snapshots after and
     * including "noon".
     */
    ret = session.checkpoint("drop=(from=noon)");

    /*
     * Checkpoint the database, discarding all snapshots before and
     * including "midnight".
     */
    ret = session.checkpoint("drop=(to=midnight)");

    /*
     * Create a checkpoint of a table, creating the "July01" snapshot and
     * discarding the "May01" and "June01" snapshots.
     * JSON parsing requires quoting the list of target URIs.
     */
    ret = session.checkpoint("target=(\"table:mytable\"),name=July01,drop=(May01,June01)");
    /*! [Checkpoint examples] */

    /*! [JSON quoting example] */
    /*
     * Checkpoint a list of objects.
     * JSON parsing requires quoting the list of target URIs.
     */
    ret = session.
        checkpoint("target=(\"table:table1\",\"table:table2\")");
    /*! [JSON quoting example] */

    return (ret);
}

static boolean
cursor_statistics(Session session)
    throws WiredTigerException
{
    Cursor cursor;

    /*! [Statistics cursor database] */
    cursor = session.open_cursor(
        "statistics:", null, null);
    /*! [Statistics cursor database] */

    /*! [Statistics cursor table] */
    cursor = session.open_cursor(
        "statistics:table:mytable", null, null);
    /*! [Statistics cursor table] */

    /*! [Statistics cursor table fast] */
    cursor = session.open_cursor("statistics:table:mytable", null, "statistics=(fast)");
    /*! [Statistics cursor table fast] */

    /*! [Statistics clear configuration] */
    cursor = session.open_cursor("statistics:", null, "statistics=(fast,clear)");
    /*! [Statistics clear configuration] */

    /*! [Statistics cursor clear configuration] */
    cursor = session.open_cursor("statistics:table:mytable",
        null, "statistics=(all,clear)");
    /*! [Statistics cursor clear configuration] */

    return (true);
}

static int
session_ops(Session session)
    throws WiredTigerException
{
    int ret;

    /*! [Reconfigure a session] */
    ret = session.reconfigure("isolation=snapshot");
    /*! [Reconfigure a session] */

    /*! [Create a table] */
    ret = session.create("table:mytable", "key_format=S,value_format=S");
    /*! [Create a table] */
    ret = session.drop("table:mytable", null);

    /*! [Create a column-store table] */
    ret = session.create("table:mytable", "key_format=r,value_format=S");
    /*! [Create a column-store table] */
    ret = session.drop("table:mytable", null);

    /*! [Create a table with columns] */
    /*
     * Create a table with columns: keys are record numbers, values are
     * (string, signed 32-bit integer, unsigned 16-bit integer).
     */
    ret = session.create("table:mytable",
        "key_format=r,value_format=SiH," +
        "columns=(id,department,salary,year-started)");
    /*! [Create a table with columns] */
    ret = session.drop("table:mytable", null);

    /*
     * This example code gets run, and the compression libraries might not
     * be loaded, causing the create to fail.  The documentation requires
     * the code snippets, use if (false) to avoid running it.
     */
    if (false) {  // MIGHT_NOT_RUN
    /*! [Create a lz4 compressed table] */
    ret = session.create("table:mytable",
        "block_compressor=lz4,key_format=S,value_format=S");
    /*! [Create a lz4 compressed table] */
    ret = session.drop("table:mytable", null);

    /*! [Create a snappy compressed table] */
    ret = session.create("table:mytable",
        "block_compressor=snappy,key_format=S,value_format=S");
    /*! [Create a snappy compressed table] */
    ret = session.drop("table:mytable", null);

    /*! [Create a zlib compressed table] */
    ret = session.create("table:mytable",
        "block_compressor=zlib,key_format=S,value_format=S");
    /*! [Create a zlib compressed table] */
    ret = session.drop("table:mytable", null);
    } // if (false)

    /*! [Configure checksums to uncompressed] */
    ret = session.create("table:mytable",
        "key_format=S,value_format=S,checksum=uncompressed");
    /*! [Configure checksums to uncompressed] */
    ret = session.drop("table:mytable", null);

    /*! [Configure dictionary compression on] */
    ret = session.create("table:mytable",
        "key_format=S,value_format=S,dictionary=1000");
    /*! [Configure dictionary compression on] */
    ret = session.drop("table:mytable", null);

    /*! [Configure key prefix compression on] */
    ret = session.create("table:mytable",
        "key_format=S,value_format=S,prefix_compression=true");
    /*! [Configure key prefix compression on] */
    ret = session.drop("table:mytable", null);

        if (false) {  // MIGHT_NOT_RUN
                        /* Requires sync_file_range */
    /*! [os_cache_dirty_max configuration] */
    ret = session.create(
        "table:mytable", "os_cache_dirty_max=500MB");
    /*! [os_cache_dirty_max configuration] */
    ret = session.drop("table:mytable", null);

                        /* Requires posix_fadvise */
    /*! [os_cache_max configuration] */
    ret = session.create("table:mytable", "os_cache_max=1GB");
    /*! [os_cache_max configuration] */
    ret = session.drop("table:mytable", null);
        } // if (false)

    /*! [Configure block_allocation] */
    ret = session.create("table:mytable",
        "key_format=S,value_format=S,block_allocation=first");
    /*! [Configure block_allocation] */
    ret = session.drop("table:mytable", null);

    /*! [Create a cache-resident object] */
    ret = session.create("table:mytable", "key_format=r,value_format=S,cache_resident=true");
    /*! [Create a cache-resident object] */
    ret = session.drop("table:mytable", null);

    {
    /* Create a table for the session operations. */
    ret = session.create(
        "table:mytable", "key_format=S,value_format=S");

    /*! [Compact a table] */
    ret = session.compact("table:mytable", null);
    /*! [Compact a table] */

    /*! [Rename a table] */
    ret = session.rename("table:old", "table:new", null);
    /*! [Rename a table] */

    /*! [Salvage a table] */
    ret = session.salvage("table:mytable", null);
    /*! [Salvage a table] */

    /*! [Truncate a table] */
    ret = session.truncate("table:mytable", null, null, null);
    /*! [Truncate a table] */

    {
    /*
     * Insert a pair of keys so we can truncate a range.
     */
    Cursor cursor;
    cursor = session.open_cursor(
        "table:mytable", null, null);
    cursor.putKeyString("June01");
    cursor.putValueString("value");
    ret = cursor.update();
    cursor.putKeyString("June30");
    cursor.putValueString("value");
    ret = cursor.update();
    cursor.close();

    {
    /*! [Truncate a range] */
    Cursor start, stop;

    start = session.open_cursor(
        "table:mytable", null, null);
    start.putKeyString("June01");
    ret = start.search();

    stop = session.open_cursor(
        "table:mytable", null, null);
    stop.putKeyString("June30");
    ret = stop.search();

    ret = session.truncate(null, start, stop, null);
    /*! [Truncate a range] */
    }
    }

    /*! [Upgrade a table] */
    ret = session.upgrade("table:mytable", null);
    /*! [Upgrade a table] */

    /*! [Verify a table] */
    ret = session.verify("table:mytable", null);
    /*! [Verify a table] */

    /*! [Drop a table] */
    ret = session.drop("table:mytable", null);
    /*! [Drop a table] */
    }

    /*! [Close a session] */
    ret = session.close(null);
    /*! [Close a session] */

    return (ret);
}

static int
transaction_ops(Connection conn, Session session)
    throws WiredTigerException
{
    Cursor cursor;
    int ret;

    /*! [transaction commit/rollback] */
    cursor = session.open_cursor("table:mytable", null, null);
    ret = session.begin_transaction(null);
    /*
     * Cursors may be opened before or after the transaction begins, and in
     * either case, subsequent operations are included in the transaction.
     * The begin_transaction call resets all open cursors.
     */

    cursor.putKeyString("key");
    cursor.putValueString("value");
    switch (ret = cursor.update()) {
    case 0:                    /* Update success */
        ret = session.commit_transaction(null);
        /*
         * The commit_transaction call resets all open cursors.
         * If commit_transaction fails, the transaction was rolled-back.
         */
        break;
    case wiredtiger.WT_ROLLBACK:            /* Update conflict */
    default:                /* Other error */
        ret = session.rollback_transaction(null);
        /* The rollback_transaction call resets all open cursors. */
        break;
    }

    /* Cursors remain open and may be used for multiple transactions. */
    /*! [transaction commit/rollback] */
    ret = cursor.close();

    /*! [transaction isolation] */
    /* A single transaction configured for snapshot isolation. */
    cursor = session.open_cursor("table:mytable", null, null);
    ret = session.begin_transaction("isolation=snapshot");
    cursor.putKeyString("some-key");
    cursor.putValueString("some-value");
    ret = cursor.update();
    ret = session.commit_transaction(null);
    /*! [transaction isolation] */

    /*! [session isolation configuration] */
    /* Open a session configured for read-uncommitted isolation. */
    session = conn.open_session(
        "isolation=read_uncommitted");
    /*! [session isolation configuration] */

    /*! [session isolation re-configuration] */
    /* Re-configure a session for snapshot isolation. */
    ret = session.reconfigure("isolation=snapshot");
    /*! [session isolation re-configuration] */

    return (ret);
}

/*! [Implement WT_COLLATOR] */
/* Not available for java */
/*! [Implement WT_COLLATOR] */

/*! [WT_EXTRACTOR] */
/* Not available for java */
/*! [WT_EXTRACTOR] */

static int
connection_ops(Connection conn)
    throws WiredTigerException
{
    int ret;

        if (false) {  // Might not run.
    /*! [Load an extension] */
    ret = conn.load_extension("my_extension.dll", null);

    ret = conn.load_extension(
        "datasource/libdatasource.so",
        "config=[device=/dev/sd1,alignment=64]");
    /*! [Load an extension] */
        } // if (false)

    /*! [Reconfigure a connection] */
    ret = conn.reconfigure("eviction_target=75");
    /*! [Reconfigure a connection] */

    /*! [Get the database home directory] */
    System.out.println("The database home is " + conn.get_home());
    /*! [Get the database home directory] */

    /*! [Check if the database is newly created] */
    if (conn.is_new() != 0) {
        /* First time initialization. */
    }
    /*! [Check if the database is newly created] */

    {
    /*! [Open a session] */
    Session session;
    session = conn.open_session(null);
    /*! [Open a session] */

    session_ops(session);
    }

    /*! [Configure method configuration] */
    /*
     * Applications opening a cursor for the data-source object "my_data"
     * have an additional configuration option "entries", which is an
     * integer type, defaults to 5, and must be an integer between 1 and 10.
     */
    ret = conn.configure_method(
        "session.open_cursor",
        "my_data:", "entries=5", "int", "min=1,max=10");

    /*
     * Applications opening a cursor for the data-source object "my_data"
     * have an additional configuration option "devices", which is a list
     * of strings.
     */
    ret = conn.configure_method(
        "session.open_cursor", "my_data:", "devices", "list", null);
    /*! [Configure method configuration] */

    /*! [Close a connection] */
    ret = conn.close(null);
    /*! [Close a connection] */

    return (ret);
}

static int
pack_ops(Session session)
{
    {
    /*! [Get the packed size] */
    /* Not available for java */
    /*! [Get the packed size] */
    }

    {
    /*! [Pack fields into a buffer] */
    /* Not available for java */
    /*! [Pack fields into a buffer] */
    }

    {
    /*! [Unpack fields from a buffer] */
    /* Not available for java */
    /*! [Unpack fields from a buffer] */
    }

    return (0);
}

static boolean
backup(Session session)
    throws WiredTigerException
{
    char buf[] = new char[1024];

    /*! [backup]*/
    Cursor cursor;
    String filename;
    int ret = 0;
        String databasedir = "/path/database";
        String backdir = "/path/database.backup";
        final String sep = File.separator;

        try {
            /* Create the backup directory. */
            if (!(new File(backdir)).mkdir()) {
                System.err.println(progname + ": cannot create backup dir: " +
                                   backdir);
                return false;
            }

            /* Open the backup data source. */
            cursor = session.open_cursor("backup:", null, null);

            /* Copy the list of files. */
            while ((ret = cursor.next()) == 0 &&
                   (filename = cursor.getKeyString()) != null) {
                String src = databasedir + sep + filename;
                String dest = backdir + sep + filename;
                java.nio.file.Files.copy(
                    new java.io.File(src).toPath(), 
                    new java.io.File(dest).toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING,
                    java.nio.file.StandardCopyOption.COPY_ATTRIBUTES);
            }
            if (ret == wiredtiger.WT_NOTFOUND)
        ret = 0;
            if (ret != 0)
                System.err.println(progname +
                   ": cursor next(backup:) failed: " +
                   wiredtiger.wiredtiger_strerror(ret));

            ret = cursor.close();
        }
        catch (Exception ex) {
            System.err.println(progname +
                ": backup failed: " + ex.toString());
        }
    /*! [backup]*/
        try {
	    /*! [incremental backup]*/
            /* Open the backup data source for incremental backup. */
            cursor = session.open_cursor("backup:", null, "target=(\"log:\")");
	    /*! [incremental backup]*/

            ret = cursor.close();
        }
        catch (Exception ex) {
            System.err.println(progname +
                ": incremental backup failed: " + ex.toString());
        }

    /*! [backup of a checkpoint]*/
    ret = session.checkpoint("drop=(from=June01),name=June01");
    /*! [backup of a checkpoint]*/

    return (ret == 0);
}

public static int
allExample()
    throws WiredTigerException
{
    Connection conn;
    int ret = 0;
    String home = "/home/example/WT_TEST";

    /*! [Open a connection] */
    conn = wiredtiger.open(home, "create,cache_size=500M");
    /*! [Open a connection] */

        connection_ops(conn);
    /*
     * The connection has been closed.
     */

    if (false) { // MIGHT_NOT_RUN
    /*
     * This example code gets run, and the compression libraries might not
     * be installed, causing the open to fail.  The documentation requires
     * the code snippets, use if (false) to avoid running it.
     */
    /*! [Configure lz4 extension] */
    conn = wiredtiger.open(home,
        "create," +
        "extensions=[/usr/local/lib/libwiredtiger_lz4.so]");
    /*! [Configure lz4 extension] */
    conn.close(null);

    /*! [Configure snappy extension] */
    conn = wiredtiger.open(home,
        "create," +
        "extensions=[/usr/local/lib/libwiredtiger_snappy.so]");
    /*! [Configure snappy extension] */
    conn.close(null);

    /*! [Configure zlib extension] */
    conn = wiredtiger.open(home,
        "create," +
        "extensions=[/usr/local/lib/libwiredtiger_zlib.so]");
    /*! [Configure zlib extension] */
    conn.close(null);

    /*
     * This example code gets run, and direct I/O might not be available,
     * causing the open to fail.  The documentation requires code snippets,
     * use if (false) to avoid running it.
     */
    /* Might Not Run: direct I/O may not be available. */
    /*! [Configure direct_io for data files] */
    conn = wiredtiger.open(home, "create,direct_io=[data]");
    /*! [Configure direct_io for data files] */
    conn.close(null);
    } // if (false)

    /*! [Configure file_extend] */
    conn = wiredtiger.open(
        home, "create,file_extend=(data=16MB)");
    /*! [Configure file_extend] */
    conn.close(null);

    /*! [Eviction configuration] */
    /*
     * Configure eviction to begin at 90% full, and run until the cache
     * is only 75% dirty.
     */
    conn = wiredtiger.open(home,
        "create,eviction_trigger=90,eviction_dirty_target=75");
    /*! [Eviction configuration] */
    conn.close(null);

    /*! [Eviction worker configuration] */
    /* Configure up to four eviction threads */
    conn = wiredtiger.open(home,
        "create,eviction_trigger=90,eviction=(threads_max=4)");
    /*! [Eviction worker configuration] */
    conn.close(null);

    /*! [Statistics configuration] */
    conn = wiredtiger.open(home, "create,statistics=(all)");
    /*! [Statistics configuration] */
    conn.close(null);

    /*! [Statistics logging] */
    conn = wiredtiger.open(
        home, "create,statistics_log=(wait=30)");
    /*! [Statistics logging] */
    conn.close(null);

    /*! [Statistics logging with a table] */
    conn = wiredtiger.open(home,
        "create," +
        "statistics_log=(sources=(\"table:table1\",\"table:table2\"))");
    /*! [Statistics logging with a table] */
    conn.close(null);

    /*! [Statistics logging with all tables] */
    conn = wiredtiger.open(home,
        "create,statistics_log=(sources=(\"table:\"))");
    /*! [Statistics logging with all tables] */
    conn.close(null);

    if (false) {  // MIGHT_NOT_RUN
    /*
     * This example code gets run, and a non-existent log file path might
     * cause the open to fail.  The documentation requires code snippets,
     * use if (false) to avoid running it.
     */
    /*! [Statistics logging with path] */
    conn = wiredtiger.open(home,
        "create," +
        "statistics_log=(wait=120,path=/log/log.%m.%d.%y)");
    /*! [Statistics logging with path] */
    conn.close(null);

    /*
     * Don't run this code, because memory checkers get very upset when we
     * leak memory.
     */
    conn = wiredtiger.open(home, "create");
    /*! [Connection close leaking memory] */
    ret = conn.close("leak_memory=true");
    /*! [Connection close leaking memory] */
    } // if (false)

    /*! [Get the WiredTiger library version #1] */
    /* Not available for java */
    /*! [Get the WiredTiger library version #1] */

    {
    /*! [Get the WiredTiger library version #2] */
    /* Not available for java */
    /*! [Get the WiredTiger library version #2] */
    }

    return (0);
}

public static int
main(String[] argv)
{
    try {
        return (allExample());
    }
    catch (WiredTigerException wte) {
        System.err.println("Exception: " + wte);
        return (-1);
    }
}
} /* Non-standard indentation */
