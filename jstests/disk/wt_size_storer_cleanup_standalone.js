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

    // TODO (SERVER-82902): Use JSON-formatted size storer data.
    // const getSizeStorerData = function() {
    //     const filePath = dbpath + (_isWindows() ? "\\" : "/") + jsTestName();
    //     runWiredTigerTool("-r", "-h", dbpath, "dump", "-j", "-f", filePath, "sizeStorer");
    //     return JSON.parse(cat(filePath))["table:sizeStorer"][1].data;
    // };
    const getSizeStorerData = function() {
        const filePath = dbpath + (_isWindows() ? "\\" : "/") + jsTestName();
        runWiredTigerTool("-r", "-h", dbpath, "dump", "-f", filePath, "sizeStorer");
        return cat(filePath);
    };

    assert.commandWorked(coll().insert({a: 1}));
    assert.eq(coll().count(), 1);
    const uri = coll().stats().wiredTiger.uri.split("statistics:")[1];

    MongoRunner.stopMongod(conn);

    let sizeStorerData = getSizeStorerData();
    // TODO (SERVER-82902): Use JSON-formatted size storer data.
    // assert(sizeStorerData.find(entry => entry.key0 === uri),
    //        "Size storer unexpectedly does not contain entry for " + uri + ": " +
    //            tojson(sizeStorerData));
    assert(sizeStorerData.includes(uri),
           "Size storer unexpectedly does not contain entry for " + uri + ": " + sizeStorerData);

    conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true, setParameter: {syncdelay: 0}});

    if (insertAfterRestart) {
        assert.commandWorked(coll().insert({a: 2}));
    }
    assert.eq(coll().count(), insertAfterRestart ? 2 : 1);
    assert(coll().drop());
    assert.commandWorked(conn.adminCommand({setParameter: 1, syncdelay: 1}));
    checkLog.containsJson(conn, 6776600, {ident: uri.split("table:")[1]});

    MongoRunner.stopMongod(conn);

    sizeStorerData = getSizeStorerData();
    // TODO (SERVER-82902): Use JSON-formatted size storer data.
    // assert(!sizeStorerData.find(entry => entry.key0 === uri),
    //        "Size storer unexpectedly contains entry for " + uri + ": " + tojson(sizeStorerData));
    assert(!sizeStorerData.includes(uri),
           "Size storer unexpectedly contains entry for " + uri + ": " + sizeStorerData);
};

runTest(false);
runTest(true);
