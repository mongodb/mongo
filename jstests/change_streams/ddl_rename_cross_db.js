/*
 * Tests that renaming a collection across databases generates
 * the correct change stream events.
 *
 * Watch all combinations of src/dest collections/db's with dropTarget true/false.
 *
 * @tags: [
 *  requires_fcv_60,
 *  # The cross db rename may not always succeed on sharded clusters if they are on different shard.
 *  assumes_against_mongod_not_mongos,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/change_stream_util.js');  // For 'ChangeStreamTest'

function runTest(watchType, renameType) {
    // Use minimum distinguishable names to keep database name under 64-byte limit.
    let dstdb = db.getSiblingDB(jsTestName() + '_dstdb_' + watchType.substr(0, 1) + '_' +
                                renameType.substr(0, 2));
    let srcdb = dstdb.getSiblingDB(jsTestName() + '_srcdb_' + watchType.substr(0, 1) + '_' +
                                   renameType.substr(0, 2));
    let dstColl = dstdb.getCollection("target");
    let srcColl = srcdb.getCollection('source');

    // Ensure dest db is non-empty before watching.
    assert.commandWorked(dstdb.unused_collection.insertOne({a: 1}));

    // Setup Change Stream Watchers
    const pipeline = [{$changeStream: {showExpandedEvents: true}}];
    const testStreamSrc = new ChangeStreamTest(srcdb);
    const testStreamDst = new ChangeStreamTest(dstdb);

    let cursorSrc;
    let cursorDst;
    if (watchType == "collection") {
        // Use doNotModifyInPassthroughs to avoid the default auto-promotion behavior of
        // changeStreamTest for the change stream to a database-level one in passthroughs.
        cursorSrc = testStreamSrc.startWatchingChanges(
            {pipeline, collection: srcColl.getName(), doNotModifyInPassthroughs: true});
        cursorDst = testStreamDst.startWatchingChanges(
            {pipeline, collection: dstColl.getName(), doNotModifyInPassthroughs: true});
    } else if (watchType == "database") {
        cursorSrc = testStreamSrc.startWatchingChanges({pipeline, collection: /*all colls=*/1});
        cursorDst = testStreamDst.startWatchingChanges({pipeline, collection: /*all colls=*/1});
    }

    if (renameType == "dropTarget" || renameType == "noDropTarget") {
        // Fill destination collection with documents and indexes
        assert.commandWorked(dstColl.insertMany([{b: "1", c: "x"}, {b: "2", c: "y"}]));
        assert.commandWorked(dstColl.createIndex({b: 1}));
        assert.commandWorked(dstColl.createIndex({c: 1}));
    }

    // Fill source collection
    assert.commandWorked(srcColl.insertMany([
        {"Key0": "Val0"},
        {"Key2": "Val1"},
        {"Key2": "Val2"},
    ]));

    // Create secondary indexes on source collection
    assert.commandWorked(srcColl.createIndex({"Key0": 1}));
    assert.commandWorked(srcColl.createIndex({"Key2": 1}));

    // Rename
    if (renameType == "newDatabase") {
        assert.commandWorked(dstdb.adminCommand(
            {renameCollection: srcColl.getFullName(), to: dstColl.getFullName()}));
    } else if (renameType == "dropTarget") {
        assert.commandWorked(dstdb.adminCommand({
            renameCollection: srcColl.getFullName(),
            to: dstColl.getFullName(),
            dropTarget: true
        }));
    } else if (renameType == "noDropTarget") {
        assert.commandFailed(dstdb.adminCommand({
            renameCollection: srcColl.getFullName(),
            to: dstColl.getFullName(),
            dropTarget: false
        }));
    }

    function eventContains(event, dbName, coll, opType, docKey, docValue) {
        let ns = event["ns"];
        let tmpRegex = RegExp("^tmp[a-zA-Z0-9]{5}[.]renameCollection$");

        if (opType != event.operationType) {
            return false;
        }

        // Operation-Specific Checks
        if (opType == "insert") {
            if (event.fullDocument[docKey] != docValue) {
                return false;
            }
        } else if (opType == "createIndexes") {
            if (event.operationDescription.indexes[0].name != docKey + "_1") {
                return false;
            }
        } else if (opType == "invalidate") {
            // This event contains no useful fields.
            return true;
        } else if (opType == "rename") {
            if (event.operationDescription.to.db != docKey ||
                event.operationDescription.to.coll != docValue) {
                return false;
            }
        }

        // Namespace Checks
        if (ns.db != dbName) {
            return false;
        }
        if (coll == "tmp") {
            if (!tmpRegex.test(ns.coll)) {
                return false;
            }
        } else if (coll != ns.coll) {
            return false;
        }

        // All important fields match.
        return true;
    }

    jsTestLog("TestType: " + watchType + " " + renameType);
    let events;

    jsTestLog("Change Streams - Src");
    jsTestLog("==========");

    // Source Collection
    events = testStreamSrc.getNextChanges(cursorSrc, 6);
    jsTestLog(events);
    assert(eventContains(events[0], srcdb.getName(), srcColl.getName(), "create", null, null));
    assert(eventContains(events[1], srcdb.getName(), srcColl.getName(), "insert", "Key0", "Val0"));
    assert(eventContains(events[2], srcdb.getName(), srcColl.getName(), "insert", "Key2", "Val1"));
    assert(eventContains(events[3], srcdb.getName(), srcColl.getName(), "insert", "Key2", "Val2"));
    assert(eventContains(
        events[4], srcdb.getName(), srcColl.getName(), "createIndexes", "Key0", null));
    assert(eventContains(
        events[5], srcdb.getName(), srcColl.getName(), "createIndexes", "Key2", null));

    if (renameType != "noDropTarget") {
        events = testStreamSrc.getNextChanges(cursorSrc, 1);
        jsTestLog(events);
        assert(eventContains(events[0], srcdb.getName(), srcColl.getName(), "drop", null, null));
    }

    jsTestLog("Change Streams - Dst");
    jsTestLog("==========");

    // Destination Database
    if (renameType == "dropTarget" || renameType == "noDropTarget") {
        events = testStreamDst.getNextChanges(cursorDst, 5);
        jsTestLog(events);
        assert(eventContains(events[0], dstdb.getName(), dstColl.getName(), "create", null, null));
        assert(eventContains(events[1], dstdb.getName(), dstColl.getName(), "insert", "b", "1"));
        assert(eventContains(events[1], dstdb.getName(), dstColl.getName(), "insert", "c", "x"));
        assert(eventContains(events[2], dstdb.getName(), dstColl.getName(), "insert", "b", "2"));
        assert(eventContains(events[2], dstdb.getName(), dstColl.getName(), "insert", "c", "y"));
        assert(eventContains(
            events[3], dstdb.getName(), dstColl.getName(), "createIndexes", "b", null));
        assert(eventContains(
            events[4], dstdb.getName(), dstColl.getName(), "createIndexes", "c", null));
    }

    if (watchType == "database" && renameType != "noDropTarget") {
        events = testStreamDst.getNextChanges(cursorDst, 6);
        jsTestLog(events);
        assert(eventContains(events[0], dstdb.getName(), "tmp", "create", null, null));
        assert(eventContains(events[1], dstdb.getName(), "tmp", "createIndexes", "Key0", null));
        assert(eventContains(events[2], dstdb.getName(), "tmp", "createIndexes", "Key2", null));
        assert(eventContains(events[3], dstdb.getName(), "tmp", "insert", "Key0", "Val0"));
        assert(eventContains(events[4], dstdb.getName(), "tmp", "insert", "Key2", "Val1"));
        assert(eventContains(events[5], dstdb.getName(), "tmp", "insert", "Key2", "Val2"));
    }

    if (renameType != "noDropTarget") {
        events = testStreamDst.getNextChanges(cursorDst, 1);
        jsTestLog(events);
        assert(eventContains(
            events[0], dstdb.getName(), "tmp", "rename", dstdb.getName(), dstColl.getName()));
    }

    if (watchType == "collection" && renameType != "noDropTarget") {
        events = testStreamDst.getNextChanges(cursorDst, 1);
        jsTestLog(events);
        assert(eventContains(events[0], null, null, "invalidate", null, null));
    }
}

let watchTypes = ["collection", "database"];
let renameTypes = ["dropTarget", "newDatabase", "noDropTarget"];
for (let watchType in watchTypes) {
    for (let renameType in renameTypes) {
        runTest(watchTypes[watchType], renameTypes[renameType]);
    }
}
})();
