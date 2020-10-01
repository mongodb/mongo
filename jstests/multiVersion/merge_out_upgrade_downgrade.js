/**
 * Tests that $merge and $out fail with a clear error message when executing against a mixed
 * verison replica set as well as a mixed version sharded cluster, but work as expected when
 * each cluster is fully upgraded.
 *
 * @tags: [requires_replication, requires_sharding]
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");        // For upgradeSecondaries and upgradeSet.
load("jstests/multiVersion/libs/multi_cluster.js");   // For upgradeCluster.
load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode.

const dbName = jsTestName();
const mergeCollName = "mergeColl";
const outCollName = "outColl";
const inputCollName = "inputColl";

/**
 * Runs 'pipeline' against 'inputColl' and verifies that it fails the $out/$merge FCV check.
 */
let runPipelineAndCheckError = function(pipeline, inputColl) {
    const error = assert.throws(
        () => inputColl.aggregate(pipeline, {$readPreference: {mode: "secondary"}}).itcount());
    assert.commandFailedWithCode(error, 31476);
};

let runPipeline = function(pipeline, inputColl) {
    assert.eq(inputColl.aggregate(pipeline, {$readPreference: {mode: "secondary"}}).itcount(), 0);
};

/**
 * Runs each valid combination of $merge modes and $out against 'inputColl' using 'execFunction'
 * to verify that either the command failed due to an FCV check or runs successfully.
 *
 * All collections created are dropped from 'targetDB' after each command completes.
 */
function runTest(execFunction, inputColl, targetDB) {
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        // Skip when whenNotMatchedMode is 'fail' since the output collection is empty, so this
        // will cause the aggregation to fail.
        if (whenNotMatchedMode === "fail") {
            return;
        }
        const mergePipeline = [{
            $merge: {
                into: mergeCollName,
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }];
        execFunction(mergePipeline, inputColl);
        targetDB[mergeCollName].drop({writeConcern: {w: 2}});
    });

    const outPipeline =
        [{$group: {_id: {$mod: ["$_id", 2]}, sum: {$sum: "$_id"}}}, {$out: outCollName}];
    execFunction(outPipeline, inputColl);
    targetDB[outCollName].drop({writeConcern: {w: 2}});
}

/**
 * Given a ReplSetTest 'rst', waits for pending changes across the cluster to complete and
 * returns the current primary and database and collection objects associated with the current
 * primary and the secondary.
 */
function awaitReplSet(rst) {
    rst.awaitSecondaryNodes();
    let primaryNode = rst.getPrimary();
    let primaryDb = primaryNode.getDB(dbName);
    let secondaryDb = rst.getSecondary().getDB(dbName);
    let inputCollPrimary = primaryDb.getCollection(inputCollName);
    let inputCollSecondary = secondaryDb.getCollection(inputCollName);
    return [primaryNode, primaryDb, secondaryDb, inputCollPrimary, inputCollSecondary];
}

// Test that $merge/$out against a secondary fails with a clear message on a mixed version replica
// set, but works as expected when the cluster is fully upgraded.
let testRepl = function() {
    const replTest = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: "last-stable"},
    });

    replTest.startSet();
    replTest.initiate();
    // Upgrade secondary to latest version.
    replTest.upgradeSecondaries({binVersion: "latest"});

    let [primary, primaryDb, secondaryDb, inputCollPrimary, inputCollSecondary] =
        awaitReplSet(replTest);

    for (let i = 0; i < 20; ++i) {
        assert.commandWorked(inputCollPrimary.insert({_id: i}, {writeConcern: {w: 2}}));
    }

    // Run $merge/$out and verify that executing against a secondary will fail when the replica set
    // cluster is not fully upgraded to v4.4.
    runTest(runPipelineAndCheckError, inputCollSecondary, primaryDb);

    // Upgrade primary to binary version 4.4.
    replTest.upgradePrimary(replTest.getPrimary(), {binVersion: "latest"});
    [primary, primaryDb, secondaryDb, inputCollPrimary, inputCollSecondary] =
        awaitReplSet(replTest);

    // The check will still fail before explicitly setting the FCV.
    runTest(runPipelineAndCheckError, inputCollSecondary, primaryDb);

    // Run $merge/$out and verify that executing against a secondary succeeds when running against a
    // fully upgraded replica set cluster.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    runTest(runPipeline, inputCollSecondary, primaryDb);

    // Downgrade the FCV to 4.2 and verify that the aggregates fail once more.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    runTest(runPipelineAndCheckError, inputCollSecondary, primaryDb);
    replTest.stopSet();
};

/**
 * Given a ShardingTest 'st', waits for pending changes across the cluster to complete and
 * returns references to the database and collection objects associated with the upgraded cluster.
 */
function awaitCluster(st) {
    st.configRS.awaitSecondaryNodes();
    st.rs0.awaitSecondaryNodes();
    st.rs1.awaitSecondaryNodes();
    st.restartMongoses();
    let mongosDb = st.s.getDB(dbName);
    return [mongosDb, mongosDb[inputCollName]];
}

// Test that $merge/$out against a secondary fails with a clear message on a mixed version sharded
// cluster, but works as expected when the cluster is fully upgraded.
let testSharding = function() {
    // Test that $merge/$out fail with a clear message on a mixed version sharded cluster.

    // One potential issue that can occur when upgrading/downgrading a single shard is that when the
    // primary is waiting on a secondary to finish upgrading and restart, the restart can take too
    // long, which causes the primary to step down and potentially have another node step up and
    // become the new primary. This causes 'upgradeSet' to fail as it assumes that the replica
    // set topology will remain unchanged during upgrade as replica set members are upgraded
    // sequentially (first the secondaries and then the primary). As such, we configure each
    // shard to have a high election timeout to avoid any unexpected topology changes during
    // the upgrade/downgrade process.
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {nodes: 2, settings: {electionTimeoutMillis: ReplSetTest.kForeverMillis}},
        other: {
            mongosOptions: {binVersion: "last-stable"},
            configOptions: {binVersion: "last-stable"},
            rsOptions: {binVersion: "last-stable"},
        }
    });

    // Ensure that shard0 is in version 4.4 and leave shard1 in version 4.2.
    st.rs0.upgradeSet({binVersion: "latest"});
    let [mongosDb, inputColl] = awaitCluster(st);

    // Enable sharding on the the test database and ensure that the primary is shard0.
    assert.commandWorked(mongosDb.adminCommand({enableSharding: mongosDb.getName()}));
    st.ensurePrimaryShard(mongosDb.getName(), st.rs0.getURL());
    st.shardColl(inputCollName, {_id: 1}, {_id: 10}, {_id: 1});

    for (let i = 0; i < 20; ++i) {
        assert.commandWorked(inputColl.insert({_id: i}));
    }

    // Verify that executing against a mixed version sharded cluster will fail.
    runTest(runPipelineAndCheckError, inputColl, mongosDb);

    // Upgrade the rest of the cluster (shard1 and mongos) to binary version 4.4.
    st.upgradeCluster("latest");
    [mongosDb, inputColl] = awaitCluster(st);

    // The check will still fail before explicitly setting the FCV.
    runTest(runPipelineAndCheckError, inputColl, mongosDb);

    // Run $merge/$out and verify that executing against a secondary succeeds when running against a
    // fully upgraded sharded cluster.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    runTest(runPipeline, inputColl, mongosDb);

    // Downgrade the FCV to 4.2 and and verify that the aggregates fail once more.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    runTest(runPipelineAndCheckError, inputColl, mongosDb);

    st.stop();
};

testRepl();
testSharding();
})();