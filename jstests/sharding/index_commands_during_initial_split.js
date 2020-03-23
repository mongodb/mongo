/*
 * Test that the index commands are correctly propagated if they are executed
 * either before, during, or after the initial split critical section.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/sharding/libs/sharded_index_util.js");

// Test intentionally inserts orphans outside of migration.
TestData.skipCheckOrphans = true;

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

const failPoints = [
    {
        name: "pauseShardCollectionBeforeCriticalSection",
        expectedAffectedShards: new Set([st.shard0, st.shard1, st.shard2]),
        criticalSectionInProgress: false
    },
    {
        name: "pauseShardCollectionReadOnlyCriticalSection",
        expectedAffectedShards: new Set([st.shard0, st.shard1, st.shard2]),
        criticalSectionInProgress: true
    },
    {
        name: "pauseShardCollectionCommitPhase",
        expectedAffectedShards: new Set([st.shard0, st.shard1, st.shard2]),
        criticalSectionInProgress: true
    },
    {
        name: "pauseShardCollectionAfterCriticalSection",
        expectedAffectedShards: new Set([st.shard1, st.shard2]),
        criticalSectionInProgress: false
    }
];

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
for (const [shardName, zone] of Object.entries(shardToZone)) {
    assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: zone.name}));
}

failPoints.forEach(failPoint => {
    jsTest.log(`Testing createIndexes in step ${failPoint.name}...`);
    const collName = "testCreateIndexes" + failPoint.name;
    const ns = dbName + "." + collName;
    const cmd = {createIndexes: collName, indexes: [index]};
    const isBlocked = failPoint.criticalSectionInProgress;

    assert.commandWorked(st.s.getDB(dbName).createCollection(collName));
    runCommandDuringShardCollection(st, ns, shardKey, zones, failPoint.name, cmd, isBlocked);

    if (isBlocked) {
        return;
    }

    // Assert that the index only exists on the targeted shards.
    allShards.forEach(shard => {
        if (failPoint.expectedAffectedShards.has(shard)) {
            ShardedIndexUtil.assertIndexExistsOnShard(shard, dbName, collName, index.key);
        } else {
            ShardedIndexUtil.assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
        }
    });
});

failPoints.forEach(failPoint => {
    jsTest.log(`Testing dropIndexes in step ${failPoint.name}...`);
    const collName = "testDropIndexes" + failPoint.name;
    const ns = dbName + "." + collName;
    const cmd = {dropIndexes: collName, index: index.name};
    const isBlocked = failPoint.criticalSectionInProgress;

    assert.commandWorked(st.s.getDB(dbName).createCollection(collName));
    assert.commandWorked(
        st.s.getDB(dbName).runCommand({createIndexes: collName, indexes: [index]}));
    runCommandDuringShardCollection(st, ns, shardKey, zones, failPoint.name, cmd, isBlocked);

    if (isBlocked) {
        return;
    }

    // Assert that the index does not exist on any shards.
    allShards.forEach(shard => {
        const usedToOwnChunks = (shard.shardName === st.shard0.shardName);
        if (failPoint.expectedAffectedShards.has(shard) || !usedToOwnChunks) {
            ShardedIndexUtil.assertIndexDoesNotExistOnShard(shard, dbName, collName, index.key);
        } else {
            ShardedIndexUtil.assertIndexExistsOnShard(st.shard0, dbName, collName, index.key);
        }
    });
});

failPoints.forEach(failPoint => {
    jsTest.log(`Testing collMod in step ${failPoint.name}...`);
    const collName = "testCollMod" + failPoint.name;
    const ns = dbName + "." + collName;
    const cmd = {collMod: collName, validator: {x: {$type: "string"}}};
    const isBlocked = failPoint.criticalSectionInProgress;

    assert.commandWorked(st.s.getDB(dbName).createCollection(collName));
    runCommandDuringShardCollection(st, ns, shardKey, zones, failPoint.name, cmd, isBlocked);

    if (isBlocked) {
        return;
    }

    // Assert that only the targeted shards do document validation.
    allShards.forEach(shard => {
        if (failPoint.expectedAffectedShards.has(shard)) {
            assert.commandFailedWithCode(shard.getCollection(ns).insert({x: 1}),
                                         ErrorCodes.DocumentValidationFailure);
        } else {
            assert.commandWorked(shard.getCollection(ns).insert({x: 1}));
        }
    });
});

st.stop();
})();
