/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build is in the drain writes phase for a single node replica set, and that the index build is
 * subsequently completed when the node is started back up.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";
const failPointName = "hangIndexBuildDuringDrainWritesPhase";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const coll = rst.getPrimary().getDB(dbName).getCollection(jsTestName());
assert.commandWorked(coll.insert({a: 1}));

ResumableIndexBuildTest.run(rst,
                            dbName,
                            coll.getName(),
                            {a: 1},
                            failPointName,
                            {iteration: 0},
                            "drain writes",
                            {skippedPhaseLogID: 20392},
                            [{a: 2}, {a: 3}],
                            [{a: 4}, {a: 5}]);
ResumableIndexBuildTest.run(rst,
                            dbName,
                            coll.getName(),
                            {a: 1},
                            failPointName,
                            {iteration: 1},
                            "drain writes",
                            {skippedPhaseLogID: 20392},
                            [{a: 6}, {a: 7}],
                            [{a: 8}, {a: 9}]);

rst.stopSet();
})();