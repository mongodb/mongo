/**
 * Test the behaviour of a dropDatabase command during an aggregation containing $out for single
 * phase index builds.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   assumes_read_concern_unchanged,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/curop_helpers.js");    // for waitForCurOpByFilter.
load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.
load("jstests/noPassthrough/libs/index_build.js");

function runTest(st, testDb, portNum) {
    const failpointName = "outWaitAfterTempCollectionCreation";
    const coll = testDb.out_source_coll;
    coll.drop();

    const targetColl = testDb.out_target_coll;
    targetColl.drop();

    assert.commandWorked(coll.insert({val: 0}));
    assert.commandWorked(coll.createIndex({val: 1}));

    let res = FixtureHelpers.runCommandOnEachPrimary({
        db: testDb.getSiblingDB("admin"),
        cmdObj: {
            configureFailPoint: failpointName,
            mode: "alwaysOn",
        }
    });

    const aggDone = startParallelShell(() => {
        const targetColl = db.getSiblingDB("out_drop_temp").out_target_coll;
        const pipeline = [{$out: "out_target_coll"}];
        targetColl.aggregate(pipeline);
    }, portNum);

    waitForCurOpByFilter(testDb, {"failpointMsg": failpointName});
    // TODO SERVER-45358 Make it easier to run commands without retrying.
    // Tests are run with an override function that retries commands that fail because of a
    // background operation. Parallel shells don't automatically have that override, so drop has to
    // be run in a parallel shell.
    const dropColl = startParallelShell(() => {
        const targetDb = db.getSiblingDB("out_drop_temp");
        assert.commandFailedWithCode(targetDb.runCommand({dropDatabase: 1}), [
            ErrorCodes.BackgroundOperationInProgressForDatabase,
            ErrorCodes.BackgroundOperationInProgressForNamespace
        ]);
    }, portNum);
    dropColl();
    // The $out should complete once the failpoint is disabled, not fail on index creation.
    FixtureHelpers.runCommandOnEachPrimary({
        db: testDb.getSiblingDB("admin"),
        cmdObj: {
            configureFailPoint: failpointName,
            mode: "off",
        }
    });
    aggDone();
}
const conn = MongoRunner.runMongod({});
if (IndexBuildTest.supportsTwoPhaseIndexBuild(conn)) {
    jsTest.log("Not running because two phase index builds are enabled.");
    MongoRunner.stopMongod(conn);
    return;
}

runTest(null, conn.getDB("out_drop_temp"), conn.port);
MongoRunner.stopMongod(conn);
const st = new ShardingTest({shards: 2, mongos: 1, config: 1});
runTest(st, st.s.getDB("out_drop_temp"), st.s.port);
st.stop();
})();
