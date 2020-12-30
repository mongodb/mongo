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
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const testCases = [
    {ns: "reshardingDb.no_compatible_index"},
    {ns: "reshardingDb.has_compatible_index", indexToCreateBeforeResharding: {newKey: 1}},
    {
        ns: "reshardingDb.compatible_index_with_extra",
        indexToCreateBeforeResharding: {newKey: 1, extra: 1}
    },
];

for (let {ns, indexToCreateBeforeResharding} of testCases) {
    const donorShardNames = reshardingTest.donorShardNames;
    const sourceCollection = reshardingTest.createShardedCollection({
        ns,
        shardKeyPattern: {oldKey: 1},
        chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    });

    if (indexToCreateBeforeResharding !== undefined) {
        assert.commandWorked(sourceCollection.createIndex(indexToCreateBeforeResharding));
    }

    // Create an index which won't be compatible with the {newKey: 1} shard key pattern but should
    // still exist post-resharding.
    assert.commandWorked(sourceCollection.createIndex({extra: 1}));
    const indexesBeforeResharding = sourceCollection.getIndexes();

    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.withReshardingInBackground({
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    });

    const indexesAfterResharding = sourceCollection.getIndexes();
    if (indexToCreateBeforeResharding !== undefined) {
        assert.sameMembers(indexesBeforeResharding, indexesAfterResharding);
    } else {
        const shardKeyIndexPos = indexesAfterResharding.findIndex(
            indexInfo => bsonBinaryEqual(indexInfo.key, {newKey: 1}));

        assert.lte(0,
                   shardKeyIndexPos,
                   `resharding didn't create index on new shard key pattern: ${
                       tojson(indexesAfterResharding)}`);

        const indexesAfterReshardingToCompare = indexesAfterResharding.slice();
        indexesAfterReshardingToCompare.splice(shardKeyIndexPos, 1);
        assert.sameMembers(indexesBeforeResharding, indexesAfterReshardingToCompare);
    }
}

reshardingTest.teardown();
})();
