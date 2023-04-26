/**
 * Tests that server fasserts on oplog applications failures in recovering mode.
 * This behavior is expected only in tests as we ignore these errors in production but output a
 * warning message.
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

// This test appends 'insert' and 'delete' oplogs directly to the oplog system collection in
// standalone mode, When restarting as a replica set member we assume the collections will play
// forward to the appropriate count. But because we added a new oplog entry that's going to turn
// into an insert, we play forward to have more documents than expected (or the reverse observation
// for adding a delete oplog entry).
TestData.skipEnforceFastCountOnValidate = true;

const ops = ["Update", "Delete", "Insert"];
const oplogApplicationResults = ["Success", "CrudError", "NamespaceNotFound"];

function runTest(op, result) {
    jsTestLog(`Testing "${op}" oplog application during recovery that finishes with "${result}".`);
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiateWithHighElectionTimeout();
    let conn = rst.getPrimary();

    // Construct a valid oplog entry.
    function constructOplogEntry(oplog) {
        const lastOplogEntry = oplog.find().sort({ts: -1}).limit(1).toArray()[0];
        const testCollOplogEntry =
            oplog.find({op: "i", ns: "test.coll"}).sort({ts: -1}).limit(1).toArray()[0];
        const highestTS = lastOplogEntry.ts;
        const oplogToInsertTS = Timestamp(highestTS.getTime(), highestTS.getInc() + 1);
        delete testCollOplogEntry.o2;
        if (op === "Delete") {
            return Object.extend(testCollOplogEntry,
                                 {op: "d", ns: "test.coll", o: {_id: 0}, ts: oplogToInsertTS});
        } else if (op === "Update") {
            return Object.extend(testCollOplogEntry, {
                op: "u",
                ns: "test.coll",
                o: {$v: 2, diff: {u: {a: 1}}},
                o2: {_id: 0},
                ts: oplogToInsertTS
            });
        } else if (op === "Insert") {
            return Object.extend(
                testCollOplogEntry,
                {op: "i", ns: "test.coll", o: {_id: 1, a: 1}, ts: oplogToInsertTS});
        }
    }

    // Do an initial insert.
    assert.commandWorked(
        conn.getDB("test").coll.insert({_id: 0, a: 0}, {writeConcern: {w: "majority"}}));
    if (result == "CrudError") {
        // For 'update' and 'delete' oplog to not find the document.
        assert.commandWorked(
            conn.getDB("test").coll.deleteOne({_id: 0}, {writeConcern: {w: "majority"}}));
        // For 'insert' to fail with duplicate key error.
        assert.commandWorked(
            conn.getDB("test").coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    } else if (result == "NamespaceNotFound") {
        conn.getDB("test").coll.drop();
    }

    jsTestLog("Restart the node in standalone mode to append the new oplog.");
    rst.stop(0, undefined /*signal*/, undefined /*opts*/, {forRestart: true});
    conn = rst.start(0, {noReplSet: true, noCleanData: true});
    let oplog = conn.getDB("local").oplog.rs;

    // Construct a valid oplog entry using the highest timestamp in the oplog. The highest timestamp
    // may differ from the one above due to concurrent internal writes when the node was a primary.
    let oplogToInsert = constructOplogEntry(oplog);
    jsTestLog(`Inserting oplog entry: ${tojson(oplogToInsert)}`);
    assert.commandWorked(oplog.insert(oplogToInsert));

    jsTestLog("Restart the node with replication enabled to let the added oplog entry" +
              " get applied as part of replication recovery.");
    rst.stop(0, undefined /*signal*/, undefined /*opts*/, {forRestart: true});
    if (result == "Success") {
        jsTestLog("The added oplog is applied successfully during replication recovery.");
        rst.start(0, {noCleanData: true});
        conn = rst.getPrimary();
        if (op === "Update") {
            assert.eq({_id: 0, a: 1}, conn.getDB("test").coll.findOne());
        } else if (op === "Delete") {
            assert.eq(null, conn.getDB("test").coll.findOne());
        } else if (op === "Insert") {
            assert.eq({_id: 0, a: 0}, conn.getDB("test").coll.findOne({_id: 0}));
            assert.eq({_id: 1, a: 1}, conn.getDB("test").coll.findOne({_id: 1}));
        }
    } else {
        jsTestLog(
            "Server should crash while applying the added oplog during replication recovery.");
        const node = rst.start(0, {noCleanData: true, waitForConnect: false});
        const exitCode = waitProgram(node.pid);
        assert.eq(exitCode, MongoRunner.EXIT_ABORT);
        assert.soon(
            function() {
                return rawMongoProgramOutput().search(/Fatal assertion.*5415000/) >= 0;
            },
            "Node should have fasserted upon encountering a fatal error during startup",
            ReplSetTest.kDefaultTimeoutMS);
    }

    rst.stopSet();
}

ops.forEach(op => oplogApplicationResults.forEach(result => runTest(op, result)));
}());
