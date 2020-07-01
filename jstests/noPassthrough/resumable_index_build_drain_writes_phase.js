/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build is in the drain writes phase, and that the index build is subsequently completed when the
 * node is started back up.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";
const collName = "resumable_index_build_drain_writes_phase";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const coll = primary.getDB(dbName).getCollection(collName);

if (!ResumableIndexBuildTest.resumableIndexBuildsEnabled(primary)) {
    jsTestLog("Skipping test because resumable index builds are not enabled");
    rst.stopSet();
    return;
}

assert.commandWorked(coll.insert({a: 1}));

ResumableIndexBuildTest.run(rst,
                            dbName,
                            collName,
                            {a: 1},
                            "hangIndexBuildDuringDrainWritesPhase",
                            {iteration: 0},
                            [{a: 2}, {a: 3}]);
ResumableIndexBuildTest.run(rst,
                            dbName,
                            collName,
                            {a: 1},
                            "hangIndexBuildDuringDrainWritesPhase",
                            {iteration: 1},
                            [{a: 4}, {a: 5}]);

rst.stopSet();
})();