/**
 * Tests that the size storer entry for a collection gets cleaned up when that collection is
 * dropped.
 *
 * @tags: [
 *   requires_wiredtiger,
 * ]
 */

import {
    runWiredTigerTool,
} from "jstests/disk/libs/wt_file_helper.js";

const runTest = function(insertAfterRestart) {
    let conn = MongoRunner.runMongod();
    const dbpath = conn.dbpath;

    const coll = function() {
        return conn.getDB(jsTestName()).test;
    };

    assert.commandWorked(coll().insert({a: 1}));
    assert.eq(coll().count(), 1);
    const uri = coll().stats().wiredTiger.uri.split("statistics:")[1];

    const assertSizeStorerEntry = function(expected) {
        const filePath = dbpath + (_isWindows() ? "\\" : "/") + jsTestName();
        runWiredTigerTool(
            "-r", "-h", dbpath, "dump", "-j", "-k", uri, "-f", filePath, "sizeStorer");
        const data = JSON.parse(cat(filePath))["table:sizeStorer"][1].data;
        assert.eq(data.length, expected ? 1 : 0, tojson(data));
    };

    MongoRunner.stopMongod(conn);
    assertSizeStorerEntry(true);

    conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true, setParameter: {syncdelay: 0}});

    if (insertAfterRestart) {
        assert.commandWorked(coll().insert({a: 2}));
    }
    assert.eq(coll().count(), insertAfterRestart ? 2 : 1);
    assert(coll().drop());
    assert.commandWorked(conn.adminCommand({setParameter: 1, syncdelay: 1}));
    checkLog.containsJson(conn, 6776600, {ident: uri.split("table:")[1]});

    MongoRunner.stopMongod(conn);
    assertSizeStorerEntry(false);
};

runTest(false);
runTest(true);
