/**
 * Ensures that the 'collStats' command lists indexes that are ready and in-progress.
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const collName = "collstats_show_ready_and_in_progress_indexes";
const testDB = db.getSiblingDB("test");
const testColl = db.getCollection(collName);
testColl.drop();

const bulk = testColl.initializeUnorderedBulkOp();
for (let i = 0; i < 5; ++i) {
    bulk.insert({a: i, b: i * i});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(
    db.adminCommand({configureFailPoint: "hangAfterStartingIndexBuildUnlocked", mode: "alwaysOn"}));

let awaitParallelShell;
try {
    jsTest.log("Starting a parallel shell to run two background index builds");
    awaitParallelShell = startParallelShell(() => {
        db.getSiblingDB("test").runCommand({
            createIndexes: "collstats_show_ready_and_in_progress_indexes",
            indexes: [
                {key: {a: 1}, name: 'a_1', background: true},
                {key: {b: 1}, name: 'b_1', background: true}
            ]
        });
    }, db.getMongo().port);

    jsTest.log("Waiting until the index build begins.");
    // Note that we cannot use checkLog here to wait for the failpoint logging because this test
    // shares a mongod with other tests that might have already provoked identical failpoint
    // logging.
    IndexBuildTest.waitForIndexBuildToScanCollection(testDB, testColl.getName(), 'b_1');

    jsTest.log("Running collStats on collection '" + collName +
               "' to check for expected 'indexSizes', 'nindexes' and 'indexBuilds' results");
    const collStatsRes = assert.commandWorked(db.runCommand({collStats: collName}));

    assert(typeof (collStatsRes.indexSizes._id_) != 'undefined',
           "expected 'indexSizes._id_' to exist: " + tojson(collStatsRes));
    assert(typeof (collStatsRes.indexSizes.a_1) != 'undefined',
           "expected 'indexSizes.a_1' to exist: " + tojson(collStatsRes));
    assert(typeof (collStatsRes.indexSizes.b_1) != 'undefined',
           "expected 'indexSizes.b_1' to exist: " + tojson(collStatsRes));

    assert.eq(3, collStatsRes.nindexes, "expected 'nindexes' to be 3: " + tojson(collStatsRes));

    assert.eq(2,
              collStatsRes.indexBuilds.length,
              "expected to find 2 entries in 'indexBuilds': " + tojson(collStatsRes));
    assert.eq('a_1',
              collStatsRes.indexBuilds[0],
              "expected to find an 'a_1' index build:" + tojson(collStatsRes));
    assert.eq('b_1',
              collStatsRes.indexBuilds[1],
              "expected to find an 'b_1' index build:" + tojson(collStatsRes));
} finally {
    // Ensure the failpoint is unset, even if there are assertion failures, so that we do not
    // hang the test/mongod.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "hangAfterStartingIndexBuildUnlocked", mode: "off"}));
    awaitParallelShell();
}
})();
