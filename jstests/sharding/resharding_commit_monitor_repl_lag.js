/**
 * Tests that the _shardsvrReshardingOperationTime command is supported on both donor and
 * recipient shard. Checks that it returns "majorityReplicationLagMillis" whether the shard is a
 * donor or recipient and only returns "elapsedMillis" and "remainingMillis" if the shard is a
 * recipient.
 *
 * This test cannot be run in config shard suites since it involves introducing replication lag
 * on all shards, and having replication lag on the config shard can cause various reads against
 * the sharding metadata collection to fail with timeout errors.
 * @tags: [
 *   config_shard_incompatible
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

// To account for other writes, when checking that there is no majority replication lag, instead
// of asserting that the lag is 0, we assert that it is less than the value below.
const maxMajorityReplicationLagMillis = 25;

function validateShardsvrReshardingOperationTimeResponse(res, isRecipient) {
    assert.eq(res.hasOwnProperty("majorityReplicationLagMillis"), true, res);
    assert.eq(res.hasOwnProperty("elapsedMillis"), isRecipient, res);
    assert.eq(res.hasOwnProperty("remainingMillis"), isRecipient, res);
}

function testShardsvrReshardingOperationTimeCmd(reshardingNs, participantRst, {isRecipient}) {
    const primary = participantRst.getPrimary();

    jsTest.log("Test the case where there is no replication lag on " + participantRst.name);
    assert.soon(() => {
        const res0 = assert.commandWorked(
            primary.adminCommand({_shardsvrReshardingOperationTime: reshardingNs}));
        jsTest.log("The latest _shardsvrReshardingOperationTime response: " + tojsononeline(res0));
        validateShardsvrReshardingOperationTimeResponse(res0, isRecipient);
        return res0.majorityReplicationLagMillis <= maxMajorityReplicationLagMillis;
    });

    jsTest.log("Test the case where there is replication lag on only one secondary on " +
               participantRst.name);
    stopServerReplication(participantRst.getSecondaries()[0]);
    const sleepMillis1 = 100;
    sleep(sleepMillis1);
    // Perform a write and and wait for it to replicate to the other secondary.
    assert.commandWorked(
        primary.adminCommand({appendOplogNote: 1, data: {replLagNoop: 0}, writeConcern: {w: 2}}));
    const res1 = assert.commandWorked(
        primary.adminCommand({_shardsvrReshardingOperationTime: reshardingNs}));
    validateShardsvrReshardingOperationTimeResponse(res1, isRecipient);
    assert.soon(() => {
        const res1 = assert.commandWorked(
            primary.adminCommand({_shardsvrReshardingOperationTime: reshardingNs}));
        jsTest.log("The latest _shardsvrReshardingOperationTime response: " + tojsononeline(res1));
        validateShardsvrReshardingOperationTimeResponse(res1, isRecipient);
        return res1.majorityReplicationLagMillis <= maxMajorityReplicationLagMillis;
    });

    jsTest.log("Test the case where there is replication lag on both secondaries on " +
               participantRst.name);
    stopServerReplication(participantRst.getSecondaries()[1]);
    const sleepMillis2 = 200;
    sleep(sleepMillis2);
    // Perform a write and and don't wait for it to replicate to secondaries since replication
    // has been paused on both secondaries.
    assert.commandWorked(
        primary.adminCommand({appendOplogNote: 1, data: {replLagNoop: 1}, writeConcern: {w: 1}}));
    const res2 = assert.commandWorked(
        primary.adminCommand({_shardsvrReshardingOperationTime: reshardingNs}));
    validateShardsvrReshardingOperationTimeResponse(res2, isRecipient);
    assert.gte(res2.majorityReplicationLagMillis, sleepMillis2, {res2});

    jsTest.log("Test the case where there is replication lag on only one secondary again on " +
               participantRst.name);
    // Unpause replication on one of the secondaries. The majority replication lag should become
    // 0 eventually.
    restartServerReplication(participantRst.getSecondaries()[0]);
    assert.soon(() => {
        const res3 = assert.commandWorked(
            primary.adminCommand({_shardsvrReshardingOperationTime: reshardingNs}));
        jsTest.log("The latest _shardsvrReshardingOperationTime response: " + tojsononeline(res3));
        validateShardsvrReshardingOperationTimeResponse(res3, isRecipient);
        return res3.majorityReplicationLagMillis <= maxMajorityReplicationLagMillis;
    });

    restartServerReplication(participantRst.getSecondaries()[1]);
}

const st = new ShardingTest({
    shards: 3,
    rs: {
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        // Disallow chaining to force both secondaries to sync from the primary. One
        // of the test cases below disables replication on one of the secondaries,
        // with chaining that would effectively disable replication on both
        // secondaries, causing the test case to to fail since writeConcern of w:
        // majority is unsatisfiable. Also, lower the heartbeat interval to reduce the time it takes
        // to wait for the majority commit point to advance.
        settings: {chainingAllowed: false, heartbeatIntervalMillis: 100},
    },
});

// Set up the collection to reshard with the following participants.
// - shard0 is a donor but not a recipient.
// - shard1 is both a donor and a recipient.
// - shard2 is a recipient but not a donor.
const reshardingDbName = "testDb";
const reshardingCollName = "testColl";
const reshardingNs = reshardingDbName + "." + reshardingCollName;
const reshardingColl = st.s.getCollection(reshardingNs);
assert.commandWorked(
    st.s.adminCommand({enableSharding: reshardingDbName, primaryShard: st.shard1.shardName}));
CreateShardedCollectionUtil.shardCollectionWithChunks(
    reshardingColl,
    {oldKey: 1},
    [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: st.shard0.shardName},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: st.shard1.shardName},
    ],
    {} /* collOpts */,
);
assert.commandWorked(
    reshardingColl.insert([
        {_id: -1, oldKey: -10, newKey: 10},
        {_id: 1, oldKey: 10, newKey: -10},
    ]),
);

function runReshardCollection(host, ns, recipientShardName0, recipientShardName1) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        _presetReshardedChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, recipientShardId: recipientShardName0},
            {min: {newKey: 0}, max: {newKey: MaxKey}, recipientShardId: recipientShardName1},
        ],
    });
}
const reshardThread = new Thread(
    runReshardCollection,
    st.s.host,
    reshardingNs,
    st.shard1.shardName,
    st.shard2.shardName,
);

const fp =
    configureFailPoint(st.configRS.getPrimary(), "reshardingPauseCoordinatorBeforeBlockingWrites");
reshardThread.start();
fp.wait();

testShardsvrReshardingOperationTimeCmd(reshardingNs, st.rs1, {isRecipient: true});
testShardsvrReshardingOperationTimeCmd(reshardingNs, st.rs2, {isRecipient: true});

fp.off();
assert.commandWorked(reshardThread.returnData());

st.stop();