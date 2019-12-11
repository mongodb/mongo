/*
 * Test that the index commands are correctly propagated if they are executed
 * either before, during, or after the initial split critical section.
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/sharding/libs/sharded_index_util.js");

/*
 * Shards the given collection.
 */
function runShardCollection(host, ns, shardKey) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({shardCollection: ns, key: shardKey});
}

/*
 * Defines zones for the given collection, then runs shardCollection and the given command after
 * the given shardCollection fail point is hit. If isBlocked is true, asserts that the command is
 * blocked (behind the initial split critical section). Otherwise, asserts that the command
 * succeeds.
 */
function runCommandDuringShardCollection(st, ns, shardKey, zones, failpointName, cmd, isBlocked) {
    const dbName = ns.split(".")[0];

    // Predefine zones for the collection.
    for (const zone of zones) {
        assert.commandWorked(st.s.adminCommand(
            {updateZoneKeyRange: ns, min: zone.min, max: zone.max, zone: zone.name}));
    }

    // Turn on the fail point on the primary shard and wait for shardCollection to hit the
    // fail point.
    let failPoint = configureFailPoint(st.shard0, failpointName);
    let shardCollThread = new Thread(runShardCollection, st.s.host, ns, shardKey);
    shardCollThread.start();
    failPoint.wait();

    if (isBlocked) {
        // Assert that the command eventually times out.
        assert.commandFailedWithCode(
            st.s.getDB(dbName).runCommand(Object.assign(cmd, {maxTimeMS: 500})),
            ErrorCodes.MaxTimeMSExpired);
    } else {
        assert.commandWorked(st.s.getDB(dbName).runCommand(cmd));
    }

    // Turn off the fail point and wait for shardCollection to complete.
    failPoint.off();
    shardCollThread.join();
    assert.commandWorked(shardCollThread.returnData());
}

const numShards = 4;
const st = new ShardingTest({shards: numShards});
const allShards = [];
for (let i = 0; i < numShards; i++) {
    allShards.push(st["shard" + i]);
}

const dbName = "test";
const testDB = st.s.getDB(dbName);
const shardKey = {
    _id: 1
};
const index = {
    key: {x: 1},
    name: "x_1"
};

const shardToZone = {
    [st.shard1.shardName]: {name: "zone1", min: {_id: MinKey}, max: {_id: 0}},
    [st.shard2.shardName]: {name: "zone2", min: {_id: 0}, max: {_id: MaxKey}}
};
const zones = Object.values(shardToZone);
const expectedTargetedShards = new Set([st.shard0, st.shard1, st.shard2]);
assert.lt(expectedTargetedShards.size, numShards);

const criticalSectionFailPointNames =
    new Set(["pauseShardCollectionReadOnlyCriticalSection", "pauseShardCollectionCommitPhase"]);
const failpointNames = [
    "pauseShardCollectionBeforeCriticalSection",
    ...criticalSectionFailPointNames,
    "pauseShardCollectionAfterCriticalSection"
];

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
for (const [shardName, zone] of Object.entries(shardToZone)) {
    assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: zone.name}));
}

failpointNames.forEach(failpointName => {
    jsTest.log(`Testing createIndexes in step ${failpointName}...`);
    const collName = "testCreateIndexes" + failpointName;
    const ns = dbName + "." + collName;
    const cmd = {createIndexes: collName, indexes: [index]};
    const isBlocked = criticalSectionFailPointNames.has(failpointName);

    assert.commandWorked(st.s.getDB(dbName).createCollection(collName));
    runCommandDuringShardCollection(st, ns, shardKey, zones, failpointName, cmd, isBlocked);

    if (isBlocked) {
        return;
    }

    // Assert that the index only exists on the targeted shards.
    allShards.forEach(shard => {
        if (expectedTargetedShards.has(shard)) {
            ShardedIndexUtil.assertIndexExistsOnShard(shard, dbName, collName, index.key);
        } else {
            ShardedIndexUtil.assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
        }
    });
});

failpointNames.forEach(failpointName => {
    jsTest.log(`Testing dropIndexes in step ${failpointName}...`);
    const collName = "testDropIndexes" + failpointName;
    const ns = dbName + "." + collName;
    const cmd = {dropIndexes: collName, index: index.name};
    const isBlocked = criticalSectionFailPointNames.has(failpointName);

    assert.commandWorked(st.s.getDB(dbName).createCollection(collName));
    assert.commandWorked(
        st.s.getDB(dbName).runCommand({createIndexes: collName, indexes: [index]}));
    runCommandDuringShardCollection(st, ns, shardKey, zones, failpointName, cmd, isBlocked);

    if (isBlocked) {
        return;
    }

    // Assert that the index does not exist on any shards.
    allShards.forEach(shard => {
        ShardedIndexUtil.assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
    });
});

failpointNames.forEach(failpointName => {
    jsTest.log(`Testing collMod in step ${failpointName}...`);
    const collName = "testCollMod" + failpointName;
    const ns = dbName + "." + collName;
    const cmd = {collMod: collName, validator: {x: {$type: "string"}}};
    const isBlocked = criticalSectionFailPointNames.has(failpointName);

    assert.commandWorked(st.s.getDB(dbName).createCollection(collName));
    runCommandDuringShardCollection(st, ns, shardKey, zones, failpointName, cmd, isBlocked);

    if (isBlocked) {
        return;
    }

    // Assert that only the targeted shards do document validation.
    allShards.forEach(shard => {
        if (expectedTargetedShards.has(shard)) {
            assert.commandFailedWithCode(shard.getCollection(ns).insert({x: 1}),
                                         ErrorCodes.DocumentValidationFailure);
        } else {
            assert.commandWorked(shard.getCollection(ns).insert({x: 1}));
        }
    });
});

st.stop();
})();
