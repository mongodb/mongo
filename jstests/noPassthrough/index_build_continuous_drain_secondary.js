/**
 * Tests that secondaries drain side writes while waiting for the primary to commit an index build.
 *
 * This test does not make very many correctness assertions because this exercises a performance
 * optimization. Instead we log the time difference between how long the primary and secondary took
 * to complete the index builds. The expectation is that these values are close to each other.
 *
 * @tags: [requires_replication]
 *
 */
(function() {
load("jstests/noPassthrough/libs/index_build.js");

const replSet = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    jsTestLog('Skipping test because two phase index builds are not supported.');
    replSet.stopSet();
    return;
}

const dbName = 'test';
const primaryDB = primary.getDB(dbName);
const coll = primaryDB.test;

let insertDocs = function(numDocs) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({a: i, b: i});
    }
    assert.commandWorked(bulk.execute());
};
insertDocs(10000);
replSet.awaitReplication();

// Start and pause the index build on the primary so that it does not start collection scanning.
IndexBuildTest.pauseIndexBuilds(primary);
const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1, b: 1});

const secondary = replSet.getSecondary();
const secondaryDB = secondary.getDB(dbName);

// Wait until the secondary reports that it is ready to commit.
// "Index build waiting for next action before completing final phase"
checkLog.containsJson(secondary, 3856203);

// Insert a high volume of documents. Since the secondary has reported that it is ready to commit,
// the expectation is that the secondary will intercept and drain these writes as they are
// replicated from primary.
insertDocs(50000);
// "index build: drained side writes"
checkLog.containsJson(secondary, 20689);

// Record how long it takes for the index build to complete from this point onward.
let start = new Date();
IndexBuildTest.resumeIndexBuilds(primary);

// Wait for index build to finish on primary.
createIdx();
let primaryEnd = new Date();

// Wait for the index build to complete on the secondary.
IndexBuildTest.waitForIndexBuildToStop(secondaryDB);
let secondaryEnd = new Date();

// We don't make any assertions about these times, just report them for informational purposes. The
// expectation is that they are as close to each other as possible, which would suggest that the
// secondary does not spend more time completing the index than the primary.
jsTestLog("these values should be similar:");
jsTestLog("elapsed on primary: " + (primaryEnd - start));
jsTestLog("elapsed on secondary: " + (secondaryEnd - start));

IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1_b_1']);
replSet.stopSet();
})();
