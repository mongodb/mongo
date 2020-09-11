/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build is in the drain writes phase for a single node replica set, and that the index build is
 * subsequently completed when the node is started back up.
 *
 * @tags: [
 *   requires_majority_read_concern,
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

const runTests = function(docs, indexSpec, sideWrites, collNameSuffix) {
    const coll = rst.getPrimary().getDB(dbName).getCollection(jsTestName() + collNameSuffix);
    assert.commandWorked(coll.insert(docs));

    const runTest = function(iteration) {
        ResumableIndexBuildTest.run(rst,
                                    dbName,
                                    coll.getName(),
                                    indexSpec,
                                    failPointName,
                                    {iteration: iteration},
                                    "drain writes",
                                    {skippedPhaseLogID: 20392},
                                    sideWrites,
                                    [{a: 4}, {a: 5}]);
    };

    runTest(0);
    runTest(1);
};

runTests({a: 1}, {a: 1}, [{a: 2}, {a: 3}], "");
runTests({a: [1, 2]}, {a: 1}, [{a: 2}, {a: 3}], "_multikey");
runTests({a: 1},
         {"$**": 1},
         [{a: [1, 2], b: {c: [3, 4]}, d: ""}, {e: "", f: [[]], g: null, h: 8}],
         "_wildcard");

rst.stopSet();
})();