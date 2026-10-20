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
 *   config_shard_incompatible,
 *   resource_intensive,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {isDisagg} from "jstests/libs/oplog_truncation_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

// To account for other writes, when checking that there is no majority replication lag, instead
// of asserting that the lag is 0, we assert that it is less than the value below.
const maxMajorityReplicationLagMillis = 25;

function validateShardsvrReshardingOperationTimeResponse(res, isRecipient) {
    // In v8.0 multiversion tests, donor-only shards do not return majorityReplicationLagMillis
    // as donor lag reporting (SERVER-104531) was not backported.
    const skipLagCheckForDonor = 
        (Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || 
         Boolean(TestData.multiversionBinVersion)) && !isRecipient;  
    if(!skipLagCheckForDonor) {
        assert.eq(res.hasOwnProperty("majorityReplicationLagMillis"), true, res);
    }

    assert.eq(res.hasOwnProperty("elapsedMillis"), isRecipient, res);
    assert.eq(res.hasOwnProperty("remainingMillis"), isRecipient, res);
}

function getShardsvrReshardingOperationTime(primary, reshardingNs, isRecipient) {
    const res = assert.commandWorked(
        primary.adminCommand({_shardsvrReshardingOperationTime: reshardingNs}));
    jsTest.log("The latest _shardsvrReshardingOperationTime response: " + tojsononeline(res));
    validateShardsvrReshardingOperationTimeResponse(res, isRecipient);
    return res;
}

function assertSoonNoMajorityReplicationLag(primary, reshardingNs, isRecipient) {
    assert.soon(() => {
        const res = getShardsvrReshardingOperationTime(primary, reshardingNs, isRecipient);
        return !res.hasOwnProperty("majorityReplicationLagMillis") ||
            res.majorityReplicationLagMillis <= maxMajorityReplicationLagMillis;
    });
}

function assertMajorityReplicationLagAtLeast(primary, reshardingNs, isRecipient, minLagMillis) {
    const res = getShardsvrReshardingOperationTime(primary, reshardingNs, isRecipient);
    if (res.hasOwnProperty("majorityReplicationLagMillis")) {
        assert.gte(res.majorityReplicationLagMillis, minLagMillis, {res});
    }
}

// Creates majority replication lag by pausing replication on the secondaries.
function testLagWithPausedSecondaries(reshardingNs, participantRst, isRecipient) {
    const primary = participantRst.getPrimary();

    jsTest.log("Test the case where there is replication lag on only one secondary on " +
               participantRst.name);
    stopServerReplication(participantRst.getSecondaries()[0]);
    const sleepMillis1 = 100;
    sleep(sleepMillis1);
    // Perform a write and and wait for it to replicate to the other secondary.
    assert.commandWorked(
        primary.adminCommand({appendOplogNote: 1, data: {replLagNoop: 0}, writeConcern: {w: 2}}));
    assertSoonNoMajorityReplicationLag(primary, reshardingNs, isRecipient);

    jsTest.log("Test the case where there is replication lag on both secondaries on " +
               participantRst.name);
    stopServerReplication(participantRst.getSecondaries()[1]);
    const sleepMillis2 = 200;
    sleep(sleepMillis2);
    // Perform a write and and don't wait for it to replicate to secondaries since replication
    // has been paused on both secondaries.
    assert.commandWorked(
        primary.adminCommand({appendOplogNote: 1, data: {replLagNoop: 1}, writeConcern: {w: 1}}));
    assertMajorityReplicationLagAtLeast(primary, reshardingNs, isRecipient, sleepMillis2);

    jsTest.log("Test the case where there is replication lag on only one secondary again on " +
               participantRst.name);
    // Unpause replication on one of the secondaries. The majority replication lag should become
    // 0 eventually.
    restartServerReplication(participantRst.getSecondaries()[0]);
    assertSoonNoMajorityReplicationLag(primary, reshardingNs, isRecipient);

    restartServerReplication(participantRst.getSecondaries()[1]);
}

// Creates majority replication lag by freezing the primary's commit point. In disaggregated
// storage the commit point is advanced by the log service rather than by secondary
// acknowledgements, so pausing secondaries does not create majority replication lag.
function testLagWithFrozenCommitPoint(reshardingNs, participantRst, isRecipient) {
    const primary = participantRst.getPrimary();

    jsTest.log("Test the case where the commit point is frozen on " + participantRst.name);
    const commitFp = configureFailPoint(primary, "hangBeforeUpdatingCommittedOpTimeFromLogServer");
    // Make a write so that a commit point update arrives and hits the failpoint. Once it is hit,
    // the commit point can no longer advance past this write.
    assert.commandWorked(
        primary.adminCommand({appendOplogNote: 1, data: {replLagNoop: 0}, writeConcern: {w: 1}}));
    commitFp.wait();
    const sleepMillis = 200;
    sleep(sleepMillis);
    // This write is applied locally but cannot be committed, so it is at least 'sleepMillis'
    // ahead of the commit point.
    assert.commandWorked(
        primary.adminCommand({appendOplogNote: 1, data: {replLagNoop: 1}, writeConcern: {w: 1}}));
    assertMajorityReplicationLagAtLeast(primary, reshardingNs, isRecipient, sleepMillis);

    jsTest.log("Test the case where the commit point is unfrozen on " + participantRst.name);
    commitFp.off();
    assertSoonNoMajorityReplicationLag(primary, reshardingNs, isRecipient);
}

function testShardsvrReshardingOperationTimeCmd(reshardingNs, participantRst, {isRecipient}) {
    const primary = participantRst.getPrimary();

    jsTest.log("Test the case where there is no replication lag on " + participantRst.name);
    assertSoonNoMajorityReplicationLag(primary, reshardingNs, isRecipient);

    if (isDisagg(primary)) {
        testLagWithFrozenCommitPoint(reshardingNs, participantRst, isRecipient);
    } else {
        testLagWithPausedSecondaries(reshardingNs, participantRst, isRecipient);
    }
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

testShardsvrReshardingOperationTimeCmd(reshardingNs, st.rs0, {isRecipient: false});
testShardsvrReshardingOperationTimeCmd(reshardingNs, st.rs1, {isRecipient: true});
testShardsvrReshardingOperationTimeCmd(reshardingNs, st.rs2, {isRecipient: true});

fp.off();
assert.commandWorked(reshardThread.returnData());

st.stop();
