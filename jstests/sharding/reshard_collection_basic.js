//
// Basic tests for reshardCollection.
// @tags: [
//   requires_fcv_47,
//   uses_atclustertime,
// ]
//

load("jstests/libs/uuid_util.js");

(function() {
'use strict';

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'db';
const collName = 'foo';
const ns = kDbName + '.' + collName;
const mongos = st.s0;
const mongosConfig = mongos.getDB('config');

let shardToRSMap = {};
shardToRSMap[st.shard0.shardName] = st.rs0;
shardToRSMap[st.shard1.shardName] = st.rs1;

let shardIdToShardMap = {};
shardIdToShardMap[st.shard0.shardName] = st.shard0;
shardIdToShardMap[st.shard1.shardName] = st.shard1;

let getUUIDFromCollectionInfo = (dbName, collName, collInfo) => {
    if (collInfo) {
        return extractUUIDFromObject(collInfo.info.uuid);
    }

    const uuidObject = getUUIDFromListCollections(mongos.getDB(dbName), collName);
    return extractUUIDFromObject(uuidObject);
};

let constructTemporaryReshardingCollName = (dbName, collName, collInfo) => {
    const existingUUID = getUUIDFromCollectionInfo(dbName, collName, collInfo);
    return 'system.resharding.' + existingUUID;
};

let getAllShardIdsFromExpectedChunks = (expectedChunks) => {
    let shardIds = new Set();
    expectedChunks.forEach(chunk => {
        shardIds.add(chunk.recipientShardId);
    });
    return shardIds;
};

let verifyTemporaryReshardingChunksMatchExpected = (expectedChunks) => {
    const tempReshardingCollNs =
        kDbName + '.' + constructTemporaryReshardingCollName(kDbName, collName);
    const tempReshardingChunks = mongosConfig.chunks.find({ns: tempReshardingCollNs}).toArray();

    expectedChunks.sort();
    tempReshardingChunks.sort();

    assert.eq(expectedChunks.size, tempReshardingChunks.size);
    for (let i = 0; i < expectedChunks.size; i++) {
        assert.eq(expectedChunks[i].recipientShardId, tempReshardingChunks[i].shard);
        assert.eq(expectedChunks[i].min, tempReshardingChunks[i].min);
        assert.eq(expectedChunks[i].max, tempReshardingChunks[i].max);
    }
};

let verifyCollectionExistenceForConn = (collName, expectedToExist, conn) => {
    const doesExist = Boolean(conn.getDB(kDbName)[collName].exists());
    assert.eq(doesExist, expectedToExist);
};

let verifyTemporaryReshardingCollectionExistsWithCorrectOptions = (expectedRecipientShards) => {
    const originalCollInfo = mongos.getDB(kDbName).getCollectionInfos({name: collName})[0];
    assert.neq(originalCollInfo, undefined);

    const tempReshardingCollName =
        constructTemporaryReshardingCollName(kDbName, collName, originalCollInfo);
    verifyCollectionExistenceForConn(tempReshardingCollName, false, mongos);

    expectedRecipientShards.forEach(shardId => {
        const rsPrimary = shardToRSMap[shardId].getPrimary();
        verifyCollectionExistenceForConn(collName, true, rsPrimary);
        verifyCollectionExistenceForConn(tempReshardingCollName, false, rsPrimary);
        ShardedIndexUtil.assertIndexExistsOnShard(
            shardIdToShardMap[shardId], kDbName, collName, {newKey: 1});
    });
};

let verifyAllShardingCollectionsRemoved = (tempReshardingCollName) => {
    assert.eq(0, mongos.getDB(kDbName)[tempReshardingCollName].find().itcount());
    assert.eq(0, mongosConfig.reshardingOperations.find({nss: ns}).itcount());
    assert.eq(0, mongosConfig.collections.find({reshardingFields: {$exists: true}}).itcount());
    assert.eq(0,
              st.rs0.getPrimary()
                  .getDB('config')
                  .localReshardingOperations.donor.find({nss: ns})
                  .itcount());
    assert.eq(0,
              st.rs0.getPrimary()
                  .getDB('config')
                  .localReshardingOperations.recipient.find({nss: ns})
                  .itcount());
    assert.eq(0,
              st.rs1.getPrimary()
                  .getDB('config')
                  .localReshardingOperations.donor.find({nss: ns})
                  .itcount());
    assert.eq(0,
              st.rs1.getPrimary()
                  .getDB('config')
                  .localReshardingOperations.recipient.find({nss: ns})
                  .itcount());
};

let assertSuccessfulReshardCollection = (commandObj, presetReshardedChunks) => {
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

    if (presetReshardedChunks) {
        commandObj._presetReshardedChunks = presetReshardedChunks;
    } else {
        assert.eq(commandObj._presetReshardedChunks, null);
        const configChunksArray = mongosConfig.chunks.find({'ns': ns});
        presetReshardedChunks = [];
        configChunksArray.forEach(chunk => {
            presetReshardedChunks.push(
                {recipientShardId: chunk.shard, min: chunk.min, max: chunk.max});
        });
    }

    assert.commandWorked(mongos.adminCommand(commandObj));

    verifyTemporaryReshardingCollectionExistsWithCorrectOptions(
        getAllShardIdsFromExpectedChunks(presetReshardedChunks));
    verifyTemporaryReshardingChunksMatchExpected(presetReshardedChunks);

    const tempReshardingCollName = constructTemporaryReshardingCollName(kDbName, collName);
    mongos.getDB(kDbName)[collName].drop();
    verifyAllShardingCollectionsRemoved(tempReshardingCollName);
};

let presetReshardedChunks =
    [{recipientShardId: st.shard1.shardName, min: {newKey: MinKey}, max: {newKey: MaxKey}}];
const existingZoneName = 'x1';

/**
 * Fail cases
 */

jsTest.log('Fail if sharding is disabled.');
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}}),
                             ErrorCodes.NamespaceNotFound);

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

jsTest.log("Fail if collection is unsharded.");
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}}),
                             ErrorCodes.NamespaceNotSharded);

assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

jsTest.log("Fail if missing required key.");
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns}), 40414);

jsTest.log("Fail if unique is specified and is true.");
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}, unique: true}),
    ErrorCodes.BadValue);

jsTest.log("Fail if collation is specified and is not {locale: 'simple'}.");
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}, collation: {locale: 'en_US'}}),
    ErrorCodes.BadValue);

jsTest.log("Fail if both numInitialChunks and _presetReshardedChunks are provided.");
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
    _presetReshardedChunks: presetReshardedChunks
}),
                             ErrorCodes.BadValue);

jsTest.log(
    "Fail if authoritative tags exist in config.tags collection and zones are not provided.");
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: existingZoneName}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {oldKey: 0}, max: {oldKey: 5}, zone: existingZoneName}));

assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

jsTest.log(
    "Fail if authoritative tags exist in config.tags collection and zones are provided and use a name which does not exist in authoritative tags.");
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{tag: 'x', min: {newKey: 5}, max: {newKey: 10}, ns: ns}],
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

/**
 * Success cases
 */

mongos.getDB(kDbName)[collName].drop();

jsTest.log("Succeed when correct locale is provided.");
assertSuccessfulReshardCollection(
    {reshardCollection: ns, key: {newKey: 1}, collation: {locale: 'simple'}});

jsTest.log("Succeed base case.");
assertSuccessfulReshardCollection({reshardCollection: ns, key: {newKey: 1}});

jsTest.log("Succeed if unique is specified and is false.");
assertSuccessfulReshardCollection({reshardCollection: ns, key: {newKey: 1}, unique: false});

jsTest.log(
    "Succeed if _presetReshardedChunks is provided and test commands are enabled (default).");
assertSuccessfulReshardCollection({reshardCollection: ns, key: {newKey: 1}}, presetReshardedChunks);

presetReshardedChunks = [
    {recipientShardId: st.shard0.shardName, min: {newKey: MinKey}, max: {newKey: 0}},
    {recipientShardId: st.shard1.shardName, min: {newKey: 0}, max: {newKey: MaxKey}}
];

jsTest.log("Succeed if all optional fields and numInitialChunks are provided with correct values.");
assertSuccessfulReshardCollection({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
});

jsTest.log(
    "Succeed if all optional fields and _presetReshardedChunks are provided with correct values and test commands are enabled (default).");
assertSuccessfulReshardCollection(
    {reshardCollection: ns, key: {newKey: 1}, unique: false, collation: {locale: 'simple'}},
    presetReshardedChunks);

jsTest.log(
    "Succeed if authoritative tags exist in config.tags collection and zones are provided and use an existing zone's name.");
assertSuccessfulReshardCollection({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{tag: existingZoneName, min: {newKey: 5}, max: {newKey: 10}, ns: ns}]
},
                                  presetReshardedChunks);

st.stop();
})();
