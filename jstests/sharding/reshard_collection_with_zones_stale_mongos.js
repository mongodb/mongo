/**
 * Tests that read/write operations through a stale mongos during each phase of reshardCollection do
 * not get blocked as long as the coordinator is not engaging the critical section.
 *
 * @tags: [
 *   requires_fcv_83,
 *   featureFlagReshardingSkipCloningAndApplyingIfApplicable,
 * ]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {isSlowBuild} from "jstests/sharding/libs/sharding_util.js";

function runReshardCollection(mongosHost, ns, key, zones) {
    const mongos = new Mongo(mongosHost);
    return mongos.adminCommand({
        reshardCollection: ns,
        key,
        zones,
        numInitialChunks: 2,
    });
}

const numShards = 3;
const numMongos = 3;
const reshardingCriticalSectionTimeoutMillis = 24 * 60 * 60 * 1_000; // 1 day

const st = new ShardingTest({
    shards: numShards,
    mongos: numMongos,
    other: {
        configOptions: {setParameter: {reshardingCriticalSectionTimeoutMillis}},
    },
});
const configPrimary = st.configRS.getPrimary();

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const failpoints = [
    "reshardingPauseCoordinatorAfterPreparingToDonate",
    "reshardingPauseCoordinatorBeforeInitializing",
    "reshardingPauseCoordinatorBeforeCloning",
    "reshardingPauseCoordinatorBeforeBlockingWrites",
    "reshardingPauseCoordinatorBeforeRemovingStateDoc",
    "reshardingPauseCoordinatorBeforeCompletion",
    "pauseBeforeTellDonorToRefresh",
    "pauseAfterInsertCoordinatorDoc",
    "pauseBeforeCTHolderInitialization",
    // TODO (SERVER-104494, SERVER-104258): Test this failpoint when enabling
    // featureFlagReshardingCloneNoRefresh.
    // "reshardingPauseBeforeTellingRecipientsToClone",
];
const waitForFailPointTimeout = kDefaultWaitForFailPointTimeout * (isSlowBuild(configPrimary) + 1);

// Setup zones for resharding.
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "zone_1"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: "zone_2"}));

function runReshardCollectionTest(failpoint) {
    jsTest.log(`Testing reshardCollection with ${tojson({failpoint})}`);

    st.s0.getCollection(ns).drop();

    const owningShard0 = st.shard0.shardName;
    // The zone for the first reshardCollection, which has shard1 as the recipient.
    let zones1 = [{zone: "zone_1", min: {y: MinKey}, max: {y: MaxKey}}];
    // The zone for the second reshardCollection, which has shard2 as the recipient.
    let zones2 = [{zone: "zone_2", min: {z: MinKey}, max: {z: MaxKey}}];

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: owningShard0}));

    // Set up a sharded collection on {x: 1} on shard0 (primary).
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(
        st.s0.getCollection(ns).insert([
            {_id: -1, x: -1, y: -1, z: -1, u: -1},
            {_id: 1, x: 1, y: 1, z: 1, u: 1},
        ]),
    );

    // Perform a read via mongos0, mongos1, and mongos2 so they have the latest routing info.
    assert.commandWorked(st.s0.getDB(dbName).runCommand({find: collName, filter: {}}));
    assert.commandWorked(st.s1.getDB(dbName).runCommand({find: collName, filter: {}}));
    assert.commandWorked(st.s2.getDB(dbName).runCommand({find: collName, filter: {}}));

    // Perform the first reshardCollection.
    let reshardThread1 = new Thread(runReshardCollection, st.s0.host, ns, {y: 1}, zones1);
    reshardThread1.start();
    reshardThread1.join();
    assert.commandWorked(reshardThread1.returnData());

    // Perform a read via mongos0 only so it has the latest routing info but mongos1 and mongos2 do
    // not.
    assert.commandWorked(st.s0.getDB(dbName).runCommand({find: collName, filter: {}}));

    // Perform the second reshardCollection.
    let fp = configureFailPoint(configPrimary, failpoint);
    let reshardThread2 = new Thread(runReshardCollection, st.s0.host, ns, {z: 1}, zones2);
    reshardThread2.start();
    fp.wait({maxTimeMS: waitForFailPointTimeout});

    jsTest.log(
        "Perform a read and write via mongos0 which has the latest routing info. " +
            "Neither of the read or write should be blocked",
    );
    assert.commandWorked(st.s0.getDB(dbName).runCommand({find: collName, filter: {}}));
    assert.commandWorked(
        st.s0.getDB(dbName).runCommand({update: collName, updates: [{q: {_id: 1}, u: {$inc: {u: 1}}}]}),
    );

    jsTest.log("Perform a read via mongos1 which has stale routing info. The read should not be blocked");
    assert.commandWorked(st.s1.getDB(dbName).runCommand({find: collName, filter: {}}));

    jsTest.log("Perform a write via mongos2 which has stale routing info. The write should not be blocked");
    // Please note that mongos1 is no longer stale after the read above.
    assert.commandWorked(
        st.s2.getDB(dbName).runCommand({update: collName, updates: [{q: {_id: 1}, u: {$inc: {u: 1}}}]}),
    );

    fp.off();
    reshardThread2.join();
    assert.commandWorked(reshardThread2.returnData());
}

// Only test a subset of failpoints to reduce test time.
const failpointRatio = 0.25;

for (let fp of failpoints) {
    if (Math.random() < failpointRatio) {
        runReshardCollectionTest(fp);
    }
}

st.stop();
