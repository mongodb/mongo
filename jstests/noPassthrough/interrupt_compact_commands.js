/**
 * Tests that the compact command is interruptible in the storage engine (WT) layer.
 * Loads data such that the storage engine compact command finds data to compress and actually runs.
 * Pauses a compact command in the MDB layer, sets interrupt via killOp, and then releases the
 * command to discover the interrupt in the storage engine layer.
 *
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

/**
 * Loads 30000 * 20 documents into collection <dbName>.<collName> via 20 threads.
 * Tags each insert with a thread ID. Then deletes half the data, by thread ID, to create holes such
 * that WT::compact finds compaction work to do.
 */
function loadData(conn, dbName, collName, coll) {
    const kThreads = 20;

    coll.createIndex({t: 1});

    jsTestLog("Loading data...");

    const threads = [];
    for (let t = 0; t < kThreads; t++) {
        let thread = new Thread(function(t, port, dbName, collName) {
            const mongo = new Mongo('localhost:' + port);
            const testDB = mongo.getDB(dbName);
            const testColl = testDB.getCollection(collName);

            // This is a sufficient amount of data for WT::compact to run. If the data size is too
            // small, WT::compact skips.
            const size = 500;
            const count = 25000;
            const doc = {a: -1, x: 'x'.repeat(size), b: -1, t: t};

            let bulkInsert = testColl.initializeUnorderedBulkOp();
            for (var i = 0; i < count; ++i) {
                bulkInsert.insert(doc);
            }
            jsTestLog("Committing inserts, t: " + t);
            assert.commandWorked(bulkInsert.execute());
        }, t, conn.port, dbName, collName);
        threads.push(thread);
        thread.start();
    }
    for (let t = 0; t < kThreads; ++t) {
        threads[t].join();
    }

    jsTestLog("Pruning data...");

    for (var t = 0; t < kThreads; t = t + 2) {
        coll.deleteMany({t: t});
    }

    jsTestLog("Data setup complete.");
}

const dbName = jsTestName();
const collName = 'testColl';

const conn = MongoRunner.runMongod();
assert.neq(conn, null);
const testDB = conn.getDB(dbName);
const testColl = testDB.getCollection(collName);

loadData(conn, dbName, collName, testColl);

let fp;
let fpOn = false;
try {
    jsTestLog("Setting the failpoint...");
    fp = configureFailPoint(testDB, "pauseCompactCommandBeforeWTCompact");
    fpOn = true;
    TestData.comment = "commentOpIdentifier";
    TestData.dbName = dbName;

    let compactJoin = startParallelShell(() => {
        jsTestLog("Starting the compact command, which should stall on a failpoint...");
        assert.commandFailedWithCode(
            db.getSiblingDB(TestData.dbName)
                .runCommand({"compact": "testColl", "comment": TestData.comment}),
            ErrorCodes.Interrupted);
    }, conn.port);

    jsTestLog("Waiting for the compact command to hit the failpoint...");
    fp.wait();

    jsTestLog("Finding the compact command opId in order to call killOp...");
    let opId = null;
    assert.soon(function() {
        const ops = testDB.getSiblingDB("admin")
                        .aggregate([
                            {$currentOp: {allUsers: true}},
                            {$match: {"command.comment": TestData.comment}}
                        ])
                        .toArray();
        if (ops.length == 0) {
            return false;
        }
        assert.eq(ops.length, 1);
        opId = ops[0].opid;
        return true;
    });
    jsTestLog("Calling killOp to interrupt the compact command, opId: " + tojson(opId));
    assert.commandWorked(testDB.killOp(opId));

    jsTestLog("Releasing the failpoint and waiting for the compact command to finish...");
    fp.off();
    fpOn = false;

    compactJoin();

    // Make sure that WT::compact did not skip because of too little data.
    assert(
        !checkLog.checkContainsOnce(testDB, "there is no useful work to do - skipping compaction"));
} finally {
    if (fpOn) {
        jsTestLog("Release the failpoint");
        fp.off();
    }
}

jsTestLog("Done");
MongoRunner.stopMongod(conn);
})();
