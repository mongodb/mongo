/**
 * Tests that the listIndexes command shows ready and in-progress indexes.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const rst = new ReplSetTest({nodes: 1});
const nodes = rst.startSet();
rst.initiate();
const conn = rst.getPrimary();

const testDB = conn.getDB("test");
assert.commandWorked(testDB.dropDatabase());

let coll = testDB.list_indexes_ready_and_in_progress;
coll.drop();
assert.commandWorked(testDB.createCollection(coll.getName()));
IndexBuildTest.assertIndexes(coll, 1, ["_id_"]);
assert.commandWorked(coll.createIndex({a: 1}));
IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

// Insert document into collection to avoid optimization for index creation on an empty collection.
// This allows us to pause index builds on the collection using a fail point.
assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(conn);
const createIdx =
    IndexBuildTest.startIndexBuild(conn, coll.getFullName(), {b: 1}, {background: true});
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'b_1');

// The listIndexes command supports returning all indexes, including ones that are not ready.
if (IndexBuildTest.supportsTwoPhaseIndexBuild(conn)) {
    IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1"], ["b_1"], {includeBuildUUIDs: true});
} else {
    IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1"], ["b_1"], {includeBuildUUIDs: false});
}

IndexBuildTest.resumeIndexBuilds(conn);

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx();
assert.eq(0, exitCode, 'expected shell to exit cleanly');

IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1", "b_1"]);
rst.stopSet();
}());
