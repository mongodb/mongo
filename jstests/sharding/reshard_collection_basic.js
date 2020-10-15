//
// Basic tests for reshardCollection.
// @tags: [requires_fcv_47]
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

let verifyTemporaryReshardingCollectionExistsWithCorrectOptionsForConn =
    (expectedCollInfo, tempCollName, conn) => {
        const tempReshardingCollInfo =
            conn.getDB(kDbName).getCollectionInfos({name: tempCollName})[0];
        assert.neq(tempReshardingCollInfo, null);
        assert.eq(expectedCollInfo.options, tempReshardingCollInfo.options);
    };

let verifyTemporaryReshardingCollectionExistsWithCorrectOptions = (expectedRecipientShards) => {
    const originalCollInfo = mongos.getDB(kDbName).getCollectionInfos({name: collName})[0];
    assert.neq(originalCollInfo, null);

    const tempReshardingCollName =
        constructTemporaryReshardingCollName(kDbName, collName, originalCollInfo);

    verifyTemporaryReshardingCollectionExistsWithCorrectOptionsForConn(
        originalCollInfo, tempReshardingCollName, mongos);

    expectedRecipientShards.forEach(shardId => {
        verifyTemporaryReshardingCollectionExistsWithCorrectOptionsForConn(
            originalCollInfo, tempReshardingCollName, shardToRSMap[shardId].getPrimary());
    });
};

let removeAllReshardingCollections = () => {
    const tempReshardingCollName = constructTemporaryReshardingCollName(kDbName, collName);
    mongos.getDB(kDbName).foo.drop();
    mongos.getDB(kDbName)[tempReshardingCollName].drop();
    mongosConfig.reshardingOperations.remove({nss: ns});
    mongosConfig.collections.remove({reshardingFields: {$exists: true}});
    st.rs0.getPrimary().getDB('config').localReshardingOperations.donor.remove({nss: ns});
    st.rs0.getPrimary().getDB('config').localReshardingOperations.recipient.remove({nss: ns});
    st.rs1.getPrimary().getDB('config').localReshardingOperations.donor.remove({nss: ns});
    st.rs1.getPrimary().getDB('config').localReshardingOperations.recipient.remove({nss: ns});
};

let assertSuccessfulReshardCollection = (commandObj, presetReshardedChunks) => {
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

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

    removeAllReshardingCollections();
};

let presetReshardedChunks =
    [{recipientShardId: st.shard1.shardName, min: {_id: MinKey}, max: {_id: MaxKey}}];
const existingZoneName = 'x1';

/**
 * Fail cases
 */

// Fail if sharding is disabled.
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {_id: 1}}),
                             ErrorCodes.NamespaceNotFound);

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

// Fail if collection is unsharded.
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {_id: 1}}),
                             ErrorCodes.NamespaceNotSharded);

assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Fail if missing required key.
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns}), 40414);

// Fail if unique is specified and is true.
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {_id: 1}, unique: true}), ErrorCodes.BadValue);

// Fail if collation is specified and is not {locale: 'simple'}.
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {_id: 1}, collation: {locale: 'en_US'}}),
    ErrorCodes.BadValue);

// Fail if both numInitialChunks and _presetReshardedChunks are provided.
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
    _presetReshardedChunks: presetReshardedChunks
}),
                             ErrorCodes.BadValue);

// Fail if authoritative tags exist in config.tags collection and zones are not provided.
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: existingZoneName}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {_id: 0}, max: {_id: 5}, zone: existingZoneName}));

assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

// Fail if authoritative tags exist in config.tags collection and zones are provided and use a name
// which does not exist in authoritative tags.
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{tag: 'x', min: {_id: 5}, max: {_id: 10}, ns: ns}],
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

/**
 * Success cases
 */

removeAllReshardingCollections();

// Succeed when correct locale is provided.
assertSuccessfulReshardCollection(
    {reshardCollection: ns, key: {_id: 1}, collation: {locale: 'simple'}});

// Succeed base case.
assertSuccessfulReshardCollection({reshardCollection: ns, key: {_id: 1}});

// Succeed if unique is specified and is false.
assertSuccessfulReshardCollection({reshardCollection: ns, key: {_id: 1}, unique: false});

// Succeed if _presetReshardedChunks is provided and test commands are enabled (default).
assertSuccessfulReshardCollection({reshardCollection: ns, key: {_id: 1}}, presetReshardedChunks);

presetReshardedChunks = [
    {recipientShardId: st.shard0.shardName, min: {_id: MinKey}, max: {_id: 0}},
    {recipientShardId: st.shard1.shardName, min: {_id: 0}, max: {_id: MaxKey}}
];

// Succeed if all optional fields and numInitialChunks are provided with correct values.
assertSuccessfulReshardCollection({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
});

// Succeed if all optional fields and _presetReshardedChunks are provided with correct values and
// test commands are enabled (default).
assertSuccessfulReshardCollection(
    {reshardCollection: ns, key: {_id: 1}, unique: false, collation: {locale: 'simple'}},
    presetReshardedChunks);

// Succeed if authoritative tags exist in config.tags collection and zones are provided and use an
// existing zone's name.
assertSuccessfulReshardCollection({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{tag: existingZoneName, min: {_id: 5}, max: {_id: 10}, ns: ns}]
},
                                  presetReshardedChunks);

st.stop();
})();
