//
// Basic tests asserting that an existing index on the new key prevents resharding from creating
// a new index.
//
// @tags: [
//   requires_fcv_49,
//   uses_atclustertime,
// ]
//

(function() {
'use strict';

load("jstests/sharding/libs/resharding_test_fixture.js");

const dbName = "reshardingDb";
const collName = "coll";
const ns = dbName + "." + collName;

let getIndexes = (dbName, collName, conn) => {
    const indexRes = conn.getDB(dbName).runCommand({listIndexes: collName});
    assert.commandWorked(indexRes);
    return indexRes.cursor.firstBatch;
};

let createShardedCollection = (reshardingTest) => {
    const donorShardNames = reshardingTest.donorShardNames;
    return reshardingTest.createShardedCollection({
        ns,
        shardKeyPattern: {oldKey: 1},
        chunks: [
            {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
            {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
        ],
    });
};

let startReshardingInBackground = (reshardingTest) => {
    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.startReshardingInBackground({
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    });
};

let awaitReshardingInState = (sourceCollection, state) => {
    const mongos = sourceCollection.getMongo();
    assert.soon(() => {
        const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne();
        return coordinatorDoc !== null && coordinatorDoc.state === state;
    });
};

let runReshardingCollectionVerifyIndexConsistency = (indexToCreateBeforeResharding) => {
    // reshardInPlace is required for this test so that the primary shard is guaranteed to know
    // about the indexes on the temporary resharding collection.
    const reshardingTest =
        new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
    reshardingTest.setup();

    let sourceCollection = createShardedCollection(reshardingTest);
    let mongos = sourceCollection.getMongo();

    let indexesBeforeResharding;
    if (indexToCreateBeforeResharding) {
        sourceCollection.createIndex(indexToCreateBeforeResharding);
        indexesBeforeResharding = getIndexes(dbName, collName, mongos);
    }

    startReshardingInBackground(reshardingTest);
    awaitReshardingInState(sourceCollection, "applying");

    if (indexToCreateBeforeResharding) {
        let indexesAfterResharding = getIndexes(dbName, collName, mongos);
        assert.sameMembers(indexesBeforeResharding, indexesAfterResharding);
    } else {
        ShardedIndexUtil.assertIndexExistsOnShard(
            mongos, dbName, reshardingTest.temporaryReshardingCollectionName, {newKey: 1});
    }

    reshardingTest.teardown();
};

runReshardingCollectionVerifyIndexConsistency();
runReshardingCollectionVerifyIndexConsistency({newKey: 1});
runReshardingCollectionVerifyIndexConsistency({newKey: 1, extra: 1});
})();
