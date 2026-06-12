/**
 * Tests that read/write operations through a stale mongos during each phase of reshardCollection
 * do not get blocked as long as the coordinator is not engaging the critical section, for
 * timeseries collections. Zone boundaries use the user-facing metaField name ("sensorId"),
 * exercising the zone range translation path.
 *
 * Note: writes use inserts rather than updates because sharded timeseries collections disallow
 * {multi:false} updates; the test only needs to verify that writes are not blocked.
 *
 * Timeseries variant of reshard_collection_with_zones_stale_mongos.js.
 *
 * @tags: [
 *   requires_fcv_90,
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
    "reshardingPauseBeforeTellingRecipientsToClone",
];
const waitForFailPointTimeout = kDefaultWaitForFailPointTimeout * (isSlowBuild(configPrimary) + 1);

// Setup zones for resharding: zone_1 will own sensorId.y ranges, zone_2 will own sensorId.z.
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "zone_1"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: "zone_2"}));

function runReshardCollectionTest(failpoint) {
    jsTest.log(`Testing reshardCollection with ${tojson({failpoint})}`);

    st.s0.getCollection(ns).drop();

    const owningShard0 = st.shard0.shardName;
    // First reshardCollection: moves to shard1 via zone_1 covering all of sensorId.y.
    // Zone boundaries use the internal "meta" field name since they are validated against the
    // translated key pattern, even though reshardCollection.key uses user-facing names.
    const zones1 = [{zone: "zone_1", min: {"meta.y": MinKey}, max: {"meta.y": MaxKey}}];
    // Second reshardCollection: moves to shard2 via zone_2 covering all of sensorId.z.
    const zones2 = [{zone: "zone_2", min: {"meta.z": MinKey}, max: {"meta.z": MaxKey}}];

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: owningShard0}));

    // Create a timeseries collection with a non-"meta" metaField name to exercise the shard key
    // translation path (user-facing field -> internal bucket field).
    assert.commandWorked(
        st.s.adminCommand({
            shardCollection: ns,
            key: {"sensorId.x": 1},
            timeseries: {timeField: "ts", metaField: "sensorId"},
        }),
    );
    assert.commandWorked(
        st.s0.getCollection(ns).insert([
            {ts: new Date(), sensorId: {x: -1, y: -1, z: -1}, v: -1},
            {ts: new Date(), sensorId: {x: 1, y: 1, z: 1}, v: 1},
        ]),
    );

    // Perform a read via mongos0, mongos1, and mongos2 so they have the latest routing info.
    assert.commandWorked(st.s0.getDB(dbName).runCommand({find: collName, filter: {}}));
    assert.commandWorked(st.s1.getDB(dbName).runCommand({find: collName, filter: {}}));
    assert.commandWorked(st.s2.getDB(dbName).runCommand({find: collName, filter: {}}));

    // Perform the first reshardCollection.
    let reshardThread1 = new Thread(
        runReshardCollection,
        st.s0.host,
        ns,
        {"sensorId.y": 1},
        zones1,
    );
    reshardThread1.start();
    reshardThread1.join();
    assert.commandWorked(reshardThread1.returnData());

    // Perform a read via mongos0 only so it has the latest routing info but mongos1 and mongos2 do
    // not.
    assert.commandWorked(st.s0.getDB(dbName).runCommand({find: collName, filter: {}}));

    // Perform the second reshardCollection.
    let fp = configureFailPoint(configPrimary, failpoint);
    let reshardThread2 = new Thread(
        runReshardCollection,
        st.s0.host,
        ns,
        {"sensorId.z": 1},
        zones2,
    );
    reshardThread2.start();
    fp.wait({maxTimeMS: waitForFailPointTimeout});

    jsTest.log(
        "Perform a read and insert via mongos0 which has the latest routing info. " +
            "Neither of the read or insert should be blocked",
    );
    assert.commandWorked(st.s0.getDB(dbName).runCommand({find: collName, filter: {}}));
    // Use insert rather than update: sharded timeseries collections disallow {multi:false} updates.
    assert.commandWorked(
        st.s0.getDB(dbName).runCommand({
            insert: collName,
            documents: [{ts: new Date(), sensorId: {x: 0, y: 0, z: 0}, v: 0}],
        }),
    );

    jsTest.log(
        "Perform a read via mongos1 which has stale routing info. The read should not be blocked",
    );
    assert.commandWorked(st.s1.getDB(dbName).runCommand({find: collName, filter: {}}));

    jsTest.log(
        "Perform an insert via mongos2 which has stale routing info. The insert should not be blocked",
    );
    // Please note that mongos1 is no longer stale after the read above.
    assert.commandWorked(
        st.s2.getDB(dbName).runCommand({
            insert: collName,
            documents: [{ts: new Date(), sensorId: {x: 0, y: 0, z: 0}, v: 0}],
        }),
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
