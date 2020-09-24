/**
 * Tests that resumable index builds that have been initialized, but not yet begun the collection
 * scan, write their state to disk upon clean shutdown and are resumed from the same phase to
 * completion when the node is started back up.
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

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const runTests = function(docs, indexSpecsFlat, collNameSuffix) {
    const coll = rst.getPrimary().getDB(dbName).getCollection(jsTestName() + collNameSuffix);
    assert.commandWorked(coll.insert(docs));

    const runTest = function(indexSpecs) {
        ResumableIndexBuildTest.run(
            rst,
            dbName,
            coll.getName(),
            indexSpecs,
            [{name: "hangIndexBuildBeforeWaitingUntilMajorityOpTime", logIdWithBuildUUID: 4940901}],
            {},
            ["initialized"],
            [{numScannedAferResume: 1}]);
    };

    runTest([[indexSpecsFlat[0]]]);
    runTest([[indexSpecsFlat[0]], [indexSpecsFlat[1]]]);
    runTest([indexSpecsFlat]);
};

runTests({a: 1, b: 1}, [{a: 1}, {b: 1}], "");
runTests({a: [1, 2], b: [1, 2]}, [{a: 1}, {b: 1}], "_multikey");
runTests({a: [1, 2], b: {c: [3, 4]}, d: ""}, [{"$**": 1}, {d: 1}], "_wildcard");

rst.stopSet();
})();