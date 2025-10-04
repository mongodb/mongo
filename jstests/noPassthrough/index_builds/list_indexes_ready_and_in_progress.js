/**
 * Tests that the listIndexes command shows ready and in-progress indexes.
 * @tags: [requires_replication]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 1});
const nodes = rst.startSet();
rst.initiate();
const conn = rst.getPrimary();

const testDB = conn.getDB("test");
assert.commandWorked(testDB.dropDatabase());

let coll = testDB[jsTestName()];
coll.drop();
assert.commandWorked(testDB.createCollection(coll.getName()));
IndexBuildTest.assertIndexes(coll, 1, ["_id_"]);
assert.commandWorked(coll.createIndex({a: 1}));
IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

// Insert document into collection to avoid optimization for index creation on an empty collection.
// This allows us to pause index builds on the collection using a fail point.
assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(conn);
const createIdx = IndexBuildTest.startIndexBuild(conn, coll.getFullName(), {b: 1});
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), "b_1");

// The listIndexes command supports returning all indexes, including ones that are not ready.
IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1"], ["b_1"], {includeBuildUUIDs: true});

IndexBuildTest.resumeIndexBuilds(conn);

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx();
assert.eq(0, exitCode, "expected shell to exit cleanly");

IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1", "b_1"]);
rst.stopSet();
