/**
 * Tests that read/write operations through a stale mongos during each phase of moveCollection do
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

function runMoveCollection(mongosHost, ns, toShard) {
    const mongos = new Mongo(mongosHost);
    return mongos.adminCommand({
        moveCollection: ns,
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

function runMoveCollectionTest(failpoint) {
    jsTest.log(`Testing moveCollection with ${tojson({failpoint})}`);

    assert.commandWorked(st.s0.getDB(dbName).dropDatabase());

    const owningShard0 = st.shard0.shardName;
    // The recipient of the first moveCollection.
    const owningShard1 = st.shard1.shardName;
    // The recipient of the second moveCollection.
    const owningShard2 = st.shard2.shardName;

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: owningShard0}));
    assert.commandWorked(st.s0.getCollection(ns).createIndex({x: 1}));
    assert.commandWorked(
        st.s0.getCollection(ns).insert([
            {_id: -1, x: -1, y: -1},
            {_id: 1, x: 1, y: 1},
        ]),
    );

    // Perform a read via mongos0, mongos1, and mongos2 so they have the latest routing info.
    st.s0.getDB(dbName).runCommand({find: collName, filter: {}});
    st.s1.getDB(dbName).runCommand({find: collName, filter: {}});
    st.s2.getDB(dbName).runCommand({find: collName, filter: {}});

    // Perform the first moveCollection.
    const moveThread1 = new Thread(runMoveCollection, st.s0.host, ns, owningShard1);
    moveThread1.start();
    moveThread1.join();
    assert.commandWorked(moveThread1.returnData());

    // Perform a read via mongos0 only so it has the latest routing info but mongos1 and mongos2 do
    // not.
    st.s0.getDB(dbName).runCommand({find: collName, filter: {}});

    // Perform the second moveCollection with a failpoint.
    const fp = configureFailPoint(configPrimary, failpoint);
    const moveThread2 = new Thread(runMoveCollection, st.s0.host, ns, owningShard2);
    moveThread2.start();
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
    moveThread2.join();
    assert.commandWorked(moveThread2.returnData());
}

// Only test a subset of failpoints to reduce test time.
const failpointRatio = 0.25;

for (let fp of failpoints) {
    if (Math.random() < failpointRatio) {
        runMoveCollectionTest(fp);
    }
}

st.stop();
