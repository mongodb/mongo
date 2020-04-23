/**
 * Perform tests for moveChunk and splitChunk commands when the shard key is compound hashed.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 2, other: {chunkSize: 1}});
const configDB = st.s0.getDB('config');
assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
st.ensurePrimaryShard('test', shard0);
const testDBOnPrimary = st.rs0.getPrimary().getDB('test');

function verifyChunkSplitIntoTwo(namespace, chunk) {
    assert.eq(0, configDB.chunks.count({ns: namespace, min: chunk.min, max: chunk.max}));
    assert.eq(1, configDB.chunks.count({ns: namespace, min: chunk.min}));
    assert.eq(1, configDB.chunks.count({ns: namespace, max: chunk.max}));
}

const nonHashedFieldValue = 111;
const hashedFieldValue = convertShardKeyToHashed(nonHashedFieldValue);

/**
 * Returns an object which has all the shard key fields. The hashed field will have the value
 * provided for 'valueForHashedField'. We are doing this because, the 'bounds' and 'middle'
 * parameters of splitChunk/moveChunk commands expect a hashed value for the hashed field, where as
 * 'find' expect a non-hashed value.
 */
function buildObjWithAllShardKeyFields(shardKey, valueForHashedField) {
    let splitObj = {};
    for (let key in shardKey) {
        if (shardKey[key] === "hashed") {
            splitObj[key] = valueForHashedField;
        } else {
            splitObj[key] = 1;
        }
    }
    return splitObj;
}

function testSplit(shardKey, collName) {
    const namespace = testDBOnPrimary[collName].getFullName();
    assert.commandWorked(configDB.adminCommand({shardCollection: namespace, key: shardKey}));

    // Insert data since both 'find' and 'bounds' based split requires the chunk to contain some
    // documents.
    const bulk = st.s.getDB("test")[collName].initializeUnorderedBulkOp();
    for (let x = -1200; x < 1200; x++) {
        bulk.insert({x: x, y: x, z: x});
    }
    assert.commandWorked(bulk.execute());

    // Attempt to split on a value that is not the shard key.
    assert.commandFailed(configDB.adminCommand({split: namespace, middle: {someField: 100}}));
    assert.commandFailed(configDB.adminCommand({split: namespace, find: {someField: 100}}));
    assert.commandFailed(configDB.adminCommand(
        {split: namespace, bounds: [{someField: MinKey}, {someField: MaxKey}]}));

    let totalChunksBefore = configDB.chunks.count({ns: namespace});
    const lowestChunk = configDB.chunks.find({ns: namespace}).sort({min: 1}).limit(1).next();
    assert(lowestChunk);
    // Split the chunk based on 'bounds' and verify total chunks increased by one.
    assert.commandWorked(
        configDB.adminCommand({split: namespace, bounds: [lowestChunk.min, lowestChunk.max]}));
    assert.eq(++totalChunksBefore, configDB.chunks.count({ns: namespace}));

    // Verify that a single chunk with the previous bounds no longer exists but split into two.
    verifyChunkSplitIntoTwo(namespace, lowestChunk);

    // Cannot split if 'min' and 'max' doesn't correspond to the same chunk.
    assert.commandFailed(
        configDB.adminCommand({split: namespace, bounds: [lowestChunk.min, lowestChunk.max]}));

    const splitObjWithHashedValue = buildObjWithAllShardKeyFields(shardKey, hashedFieldValue);

    // Find the chunk to which 'splitObjWithHashedValue' belongs to.
    let chunkToBeSplit = configDB.chunks.findOne(
        {ns: namespace, min: {$lte: splitObjWithHashedValue}, max: {$gt: splitObjWithHashedValue}});
    assert(chunkToBeSplit);

    // Split the 'chunkToBeSplit' using 'find'. Note that the object specified for 'find' is not a
    // split point.
    const splitObj = buildObjWithAllShardKeyFields(shardKey, nonHashedFieldValue);
    assert.commandWorked(configDB.adminCommand({split: namespace, find: splitObj}));
    assert.eq(++totalChunksBefore, configDB.chunks.count({ns: namespace}));

    // Verify that a single chunk with the previous bounds no longer exists but split into two.
    verifyChunkSplitIntoTwo(namespace, chunkToBeSplit);
    assert.eq(0, configDB.chunks.count({ns: namespace, min: splitObjWithHashedValue}));

    // Get the new chunk in which 'splitObj' belongs.
    chunkToBeSplit = configDB.chunks.findOne(
        {ns: namespace, min: {$lte: splitObjWithHashedValue}, max: {$gt: splitObjWithHashedValue}});

    // Use 'splitObj' as the middle point.
    assert.commandWorked(
        configDB.adminCommand({split: namespace, middle: splitObjWithHashedValue}));
    assert.eq(++totalChunksBefore, configDB.chunks.count({ns: namespace}));
    verifyChunkSplitIntoTwo(namespace, chunkToBeSplit);

    // Cannot split on existing chunk boundary with 'middle'.
    assert.commandFailed(configDB.adminCommand({split: namespace, middle: chunkToBeSplit.min}));

    st.s.getDB("test")[collName].drop();
}

testSplit({x: "hashed", y: 1, z: 1}, "compound_hashed_prefix");
testSplit({_id: "hashed", y: 1, z: 1}, "compound_hashed_prefix_id");
testSplit({x: 1, y: "hashed", z: 1}, "compound_nonhashed_prefix");
testSplit({x: 1, _id: "hashed", z: 1}, "compound_nonhashed_prefix_id");

function testMoveChunk(shardKey) {
    const ns = 'test.fooHashed';
    assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: shardKey}));

    // Fetch a chunk from 'shard0'.
    const aChunk = configDB.chunks.findOne({ns: ns, shard: shard0});
    assert(aChunk);

    // Error if either of the bounds is not a valid shard key.
    assert.commandFailedWithCode(
        st.s0.adminCommand({moveChunk: ns, bounds: [NaN, aChunk.max], to: shard1}), 10065);
    assert.commandFailedWithCode(
        st.s0.adminCommand({moveChunk: ns, bounds: [aChunk.min, NaN], to: shard1}), 10065);

    assert.commandWorked(
        st.s0.adminCommand({moveChunk: ns, bounds: [aChunk.min, aChunk.max], to: shard1}));

    assert.eq(0, configDB.chunks.count({_id: aChunk._id, shard: shard0}));
    assert.eq(1, configDB.chunks.count({_id: aChunk._id, shard: shard1}));

    // Fail if 'find' doesn't have full shard key.
    assert.commandFailed(st.s0.adminCommand({moveChunk: ns, find: {someField: 0}, to: shard1}));

    // Find the chunk to which 'moveObjWithHashedValue' belongs to.
    const moveObjWithHashedValue = buildObjWithAllShardKeyFields(shardKey, hashedFieldValue);
    const chunk = st.config.chunks.findOne(
        {ns: ns, min: {$lte: moveObjWithHashedValue}, max: {$gt: moveObjWithHashedValue}});
    assert(chunk);

    // Verify that 'moveChunk' with 'find' works with pre-hashed value.
    const otherShard = (chunk.shard === shard1) ? shard0 : shard1;
    const moveObj = buildObjWithAllShardKeyFields(shardKey, nonHashedFieldValue);
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: moveObj, to: otherShard}));
    assert.eq(st.config.chunks.count({ns: ns, min: chunk.min, shard: otherShard}), 1);

    // Fail if 'find' and 'bounds' are both set.
    assert.commandFailed(st.s0.adminCommand({
        moveChunk: ns,
        find: moveObjWithHashedValue,
        bounds: [aChunk.min, aChunk.max],
        to: shard1
    }));

    st.s.getDB("test").fooHashed.drop();
}

testMoveChunk({_id: "hashed", b: 1, c: 1});
testMoveChunk({_id: 1, "b.c.d": "hashed", c: 1});

st.stop();
})();
