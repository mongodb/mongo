/**
 * This test verifies that calling resharding on a collection with:
 * - a non-simple default collation and
 * - a secondary index spec with a simple collation
 * results in a collection with a secondary index with a simple collation.
 * (SERVER-89744 for more info).
 *
 */
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest();

reshardingTest.setup();
const donorShardNames = reshardingTest.donorShardNames;

const kDbName = "reshardingDb";
const kCollName = "coll";
const ns = kDbName + "." + kCollName;

assert.commandWorked(reshardingTest._st.s.getDB(kDbName).adminCommand(
    {enableSharding: kDbName, primaryShard: donorShardNames[0]}));

assert.commandWorked(reshardingTest._st.s.getCollection(ns).runCommand(
    "create", {collation: {locale: "en_US", strength: 2}}));

const collection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    collOptions: {collation: {locale: "simple"}},
});

const idxSimpleCollationName = "idxSimpleCollation";
assert.commandWorked(
    collection.createIndex({x: 1}, {name: idxSimpleCollationName, collation: {locale: "simple"}}));
const idx2Name = "idx2";
assert.commandWorked(collection.createIndex({x: 1}, {name: idx2Name}));

const preReshardingIndexes = collection.getIndexes();
const preIdxDict = {};
preReshardingIndexes.forEach(function(idx) {
    preIdxDict[idx.name] = idx;
});

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
});

const postReshardingIndexes = collection.getIndexes();
assert.eq(postReshardingIndexes.length, 5);

for (const postIdxSpec of postReshardingIndexes) {
    if ('newKey' in postIdxSpec.key) {
        // the index collation for post resharding key should be {locale: "simple"}. listIndexes
        // omits this collation.
        assert.eq(postIdxSpec.collation, undefined);
    } else {
        assert.eq(postIdxSpec, preIdxDict[postIdxSpec.name]);
    }
}

reshardingTest.teardown();
