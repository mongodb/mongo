/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build has been initialized but has not yet begun the collection scan phase.
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
const failPointName = "hangIndexBuildBeforeWaitingUntilMajorityOpTime";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const runTests = function(docs, indexSpec, collNameSuffix) {
    const coll = rst.getPrimary().getDB(dbName).getCollection(jsTestName() + collNameSuffix);
    assert.commandWorked(coll.insert(docs));

    ResumableIndexBuildTest.run(
        rst, dbName, coll.getName(), indexSpec, failPointName, {}, "initialized", {
            numScannedAferResume: 1
        });
};

runTests({a: 1}, {a: 1}, "");
runTests({a: [1, 2]}, {a: 1}, "_multikey");
runTests({a: [1, 2], b: {c: [3, 4]}, d: ""}, {"$**": 1}, "_wildcard");

rst.stopSet();
})();