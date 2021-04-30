/**
 * Capped cursors return CappedPositionLost when the document they were positioned on gets deleted.
 * When this occurs during the collection scan phase of an index build, it will get restarted.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/noPassthrough/libs/index_build.js");

const conn = MongoRunner.runMongod({});

const dbName = "test";
const collName = "index_build_capped_position_lost";

const db = conn.getDB(dbName);
assert.commandWorked(
    db.createCollection(collName, {capped: true, size: 1024 * 1024 * 1024, max: 5}));

const coll = db.getCollection(collName);

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Hang the collection scan phase of the index build when it's halfway finished.
let fp = configureFailPoint(
    conn, "hangIndexBuildDuringCollectionScanPhaseAfterInsertion", {fieldsToMatch: {a: 3}});

const awaitCreateIndex = IndexBuildTest.startIndexBuild(conn, coll.getFullName(), {a: 1});
fp.wait();

// Rollover the capped collection.
for (let i = 5; i < 10; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

fp.off();
checkLog.containsJson(conn, 5470300, {
    error: function(error) {
        return error.code === ErrorCodes.CappedPositionLost;
    }
});                                                     // Collection scan restarted.
checkLog.containsJson(conn, 20391, {totalRecords: 5});  // Collection scan complete.

awaitCreateIndex();

IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

MongoRunner.stopMongod(conn);
}());
