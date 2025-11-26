/**
 * Tests that read/write operations through a stale mongos during each phase of unshardCollection do
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

function runUnshardCollection(mongosHost, ns, toShard) {
    const mongos = new Mongo(mongosHost);
    return mongos.adminCommand({
        unshardCollection: ns,
        toShard,
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

function runUnshardCollectionTest(failpoint) {
    jsTest.log(`Testing unshardCollection with ${tojson({failpoint})}`);

    assert.commandWorked(st.s0.getDB(dbName).dropDatabase());

    const owningShard0 = st.shard0.shardName;
    // The recipient for the first unshardCollection.
    const owningShard1 = st.shard1.shardName;
    // The recipient for the second unshardCollection.
    const owningShard2 = st.shard2.shardName;

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: owningShard0}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(
        st.s0.getCollection(ns).insert([
            {_id: -1, x: -1, y: -1},
            {_id: 1, x: 1, y: 1},
        ]),
    );

    // Perform a read via mongos0, mongos1, and mongos2 so they have the latest routing info.
    assert.commandWorked(st.s0.getDB(dbName).runCommand({find: collName, filter: {}}));
    assert.commandWorked(st.s1.getDB(dbName).runCommand({find: collName, filter: {}}));
    assert.commandWorked(st.s2.getDB(dbName).runCommand({find: collName, filter: {}}));

    // Perform the first unshardCollection.
    let unshardThread1 = new Thread(runUnshardCollection, st.s0.host, ns, owningShard1);
    unshardThread1.start();
    unshardThread1.join();
    assert.commandWorked(unshardThread1.returnData());

    // Perform a read via mongos0 only so it has the latest routing info but mongos1 and mongos2 do
    // not.
    assert.commandWorked(st.s0.getDB(dbName).runCommand({find: collName, filter: {}}));

    // Re-shard the collection to test another unshardCollection operation.
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

    // Perform the second unshardCollection with a failpoint.
    let fp = configureFailPoint(st.configRS.getPrimary(), failpoint);
    let unshardThread2 = new Thread(runUnshardCollection, st.s0.host, ns, owningShard2);
    unshardThread2.start();
    fp.wait({maxTimeMS: waitForFailPointTimeout});

    jsTest.log(
        "Perform a read and write via mongos0 which has the latest routing info. " +
            "Neither of the read or write should be blocked",
    );
    assert.commandWorked(st.s0.getDB(dbName).runCommand({find: collName, filter: {}}));
    assert.commandWorked(
        st.s0.getDB(dbName).runCommand({update: collName, updates: [{q: {_id: 1}, u: {$inc: {y: 1}}}]}),
    );

    jsTest.log("Perform a read via mongos1 which has stale routing info. The read should not be blocked");
    assert.commandWorked(st.s1.getDB(dbName).runCommand({find: collName, filter: {}}));

    jsTest.log("Perform a write via mongos2 which has stale routing info. The write should not be blocked");
    // Please note that mongos1 is no longer stale after the read above.
    assert.commandWorked(
        st.s2.getDB(dbName).runCommand({update: collName, updates: [{q: {_id: 1}, u: {$inc: {y: 1}}}]}),
    );

    fp.off();
    unshardThread2.join();
    assert.commandWorked(unshardThread2.returnData());
}

// Only test a subset of failpoints to reduce test time.
const failpointRatio = 0.25;

for (let fp of failpoints) {
    if (Math.random() < failpointRatio) {
        runUnshardCollectionTest(fp);
    }
}

st.stop();
