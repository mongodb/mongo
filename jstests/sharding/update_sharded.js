/**
 * Test simple updates issued through mongos. Updates have different constraints through mongos,
 * since shard key is immutable.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */
import {
    enableCoordinateCommitReturnImmediatelyAfterPersistingDecision
} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {
    WriteWithoutShardKeyTestUtil
} from "jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js";

const s = new ShardingTest({name: "auto1", shards: 2, mongos: 1});

enableCoordinateCommitReturnImmediatelyAfterPersistingDecision(s);
s.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName});

const dbName = "test";
const db = s.getDB(dbName);
const sessionDb = s.s.startSession({retryWrites: true}).getDatabase("test");

s.shard0.getDB("admin").setLogLevel(1);
s.shard1.getDB("admin").setLogLevel(1);

function testShardKeys(collName, hashedKey) {
    const coll = db.getCollection(collName);
    const sessionColl = sessionDb.getCollection(collName);
    const keyPattern = hashedKey ? {key: "hashed"} : {key: 1};

    // Create an index on the shard key pattern. Then add a single document to the collection
    // so that it's not empty.
    coll.createIndex(keyPattern);
    coll.insert({_id: -1, key: -999});

    // Shard the collection, split it at "key = 0" (for ascending shard keys) or at "hash(key) = 0"
    // (for hashed shard keys), and finally take the chunk that owns "key" / "hash(key)" values
    // greater than or equal to zero and move that chunk to shard0.
    const middle = hashedKey ? {key: NumberLong(0)} : {key: 0};
    s.shardColl(collName, keyPattern, middle, {key: 999}, dbName, true);

    // Remove all documents from the collection before executing the testcases below.
    coll.remove({});

    // Insert a single document.
    coll.insert({_id: 1, key: 1});

    // Replacment and Opstyle upserts.
    assert.commandWorked(coll.update({_id: 2, key: 2}, {key: 2, foo: 'bar'}, {upsert: true}));
    assert.commandWorked(coll.update({_id: 3, key: 3}, {$set: {foo: 'bar'}}, {upsert: true}));

    // Mixing operator & non-operator fields in updates is not allowed.
    assert.commandFailedWithCode(
        coll.update({_id: 4, key: 4}, {key: 4, $baz: {foo: 'bar'}}, {upsert: true}),
        ErrorCodes.UnsupportedFormat);
    assert.commandFailedWithCode(
        coll.update({_id: 5, key: 5}, {$baz: {foo: 'bar'}, key: 5}, {upsert: true}),
        ErrorCodes.UnsupportedFormat);

    assert.eq(coll.count(), 3, "count A");
    assert.eq(coll.findOne({_id: 3}).key, 3, "findOne 3 key A");
    assert.eq(coll.findOne({_id: 3}).foo, 'bar', "findOne 3 foo A");

    // update existing using update()
    assert.commandWorked(coll.update({_id: 1}, {key: 1, other: 1}));
    assert.commandWorked(coll.update({_id: 2}, {key: 2, other: 2}));
    assert.commandWorked(coll.update({_id: 3}, {key: 3, other: 3}));

    // do a replacement-style update which queries the shard key and keeps it constant
    assert.commandWorked(coll.update({key: 4}, {_id: 4, key: 4}, {upsert: true}));
    assert.commandWorked(coll.update({key: 4}, {key: 4, other: 4}));
    assert.eq(coll.find({key: 4, other: 4}).count(), 1, 'replacement update error');
    coll.remove({_id: 4});

    assert.eq(coll.count(), 3, "count B");
    coll.find().forEach(function(x) {
        assert.eq(x._id, x.key, "_id == key");
        assert.eq(x._id, x.other, "_id == other");
    });

    // {key:1} and {key:2} will belong to shard0 for both hashed and ascending shard keys.
    // Therefore these two "update" commands should execute as normal (non-WCOS) updates.
    assert.commandWorked(sessionColl.update({_id: 1, key: 1}, {$set: {key: 2}}));
    assert.eq(coll.findOne({_id: 1}).key, 2, 'key changed');
    assert.commandWorked(
        sessionColl.update({_id: 1, key: 2}, {$set: {key: 1}}));  // Reset the key value.

    assert.commandWorked(coll.update({_id: 1, key: 1}, {$set: {foo: 2}}));

    coll.update({key: 17}, {$inc: {x: 5}}, true);
    assert.eq(5, coll.findOne({key: 17}).x, "up1");

    coll.update({key: 18}, {$inc: {x: 5}}, true, true);
    assert.eq(5, coll.findOne({key: 18}).x, "up2");

    // Make sure we can extract exact _id from certain queries
    assert.commandWorked(coll.update({_id: ObjectId()}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({_id: {$eq: ObjectId()}}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({_id: {$all: [ObjectId()]}}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({$or: [{_id: ObjectId()}]}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({$and: [{_id: ObjectId()}]}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({_id: {$in: [ObjectId()]}}, {$set: {x: 1}}, {multi: false}));

    // Run the two phase broadcast write protocol for multi: false writes without a proper _id or
    // shard key to target by.
    if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(sessionDb)) {
        assert.commandWorked(coll.update({}, {$set: {x: 1}}, {multi: false}));
        assert.commandWorked(coll.update({_id: {$gt: ObjectId()}}, {$set: {x: 1}}, {multi: false}));
        assert.commandWorked(coll.update(
            {$or: [{_id: ObjectId()}, {_id: ObjectId()}]}, {$set: {x: 1}}, {multi: false}));
        assert.commandWorked(coll.update(
            {$and: [{_id: ObjectId()}, {_id: ObjectId()}]}, {$set: {x: 1}}, {multi: false}));
        assert.commandWorked(coll.update({'_id.x': ObjectId()}, {$set: {x: 1}}, {multi: false}));
    } else {
        // Invalid extraction of exact _id from query
        assert.commandFailedWithCode(coll.update({}, {$set: {x: 1}}, {multi: false}),
                                     ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(
            coll.update({_id: {$gt: ObjectId()}}, {$set: {x: 1}}, {multi: false}),
            ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(
            coll.update(
                {$or: [{_id: ObjectId()}, {_id: ObjectId()}]}, {$set: {x: 1}}, {multi: false}),
            ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(
            coll.update(
                {$and: [{_id: ObjectId()}, {_id: ObjectId()}]}, {$set: {x: 1}}, {multi: false}),
            ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(
            coll.update({'_id.x': ObjectId()}, {$set: {x: 1}}, {multi: false}),
            ErrorCodes.InvalidOptions);
    }

    // Make sure we can extract exact shard key from certain queries
    assert.commandWorked(coll.update({key: ObjectId()}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({key: {$eq: ObjectId()}}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({key: {$in: [ObjectId()]}}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({key: {$all: [ObjectId()]}}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({$or: [{key: ObjectId()}]}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(coll.update({$and: [{key: ObjectId()}]}, {$set: {x: 1}}, {multi: false}));

    // Run the two phase broadcast write protocol for multi: false writes without a proper _id or
    // shard key to target by.
    if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(sessionDb)) {
        assert.commandWorked(coll.update({}, {$set: {x: 1}}, {multi: false}));
        assert.commandWorked(coll.update({'key.x': ObjectId()}, {$set: {x: 1}}, {multi: false}));
    } else {
        // Invalid extraction of exact key from query
        assert.commandFailedWithCode(coll.update({}, {$set: {x: 1}}, {multi: false}),
                                     ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(
            coll.update({'key.x': ObjectId()}, {$set: {x: 1}}, {multi: false}),
            ErrorCodes.InvalidOptions);
    }

    // Run the two phase broadcast write protocol for multi: false writes without a proper _id or
    // shard key to target by.
    if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(sessionDb)) {
        assert.commandWorked(coll.update({key: {$gt: 0}}, {$set: {x: 1}}, {multi: false}));
    } else {
        // Inexact queries may target a single shard. Range queries may target a single shard as
        // long as the collection is not hashed.
        if (hashedKey) {
            assert.commandFailedWithCode(
                coll.update({key: {$gt: 0}}, {$set: {x: 1}}, {multi: false}),
                ErrorCodes.InvalidOptions);
        } else {
            assert.commandWorked(coll.update({key: {$gt: 0}}, {$set: {x: 1}}, {multi: false}));
        }
    }
    // Note: {key:-1} and {key:-2} fall on the same shard for both hashed and ascending shardkeys.
    assert.commandWorked(
        coll.update({$or: [{key: -1}, {key: -2}]}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(
        coll.update({$and: [{key: -1}, {key: -2}]}, {$set: {x: 1}}, {multi: false}));

    // Run the two phase broadcast write protocol for multi: false writes without a proper _id or
    // shard key to target by.
    if (WriteWithoutShardKeyTestUtil.isWriteWithoutShardKeyFeatureEnabled(sessionDb)) {
        assert.commandWorked(coll.update({key: {$gt: MinKey}}, {$set: {x: 1}}, {multi: false}));
        assert.commandWorked(
            coll.update({$or: [{key: -10}, {key: 10}]}, {$set: {x: 1}}, {multi: false}));
    } else {
        // In cases where an inexact query does target multiple shards, single update is rejected.
        assert.commandFailedWithCode(
            coll.update({key: {$gt: MinKey}}, {$set: {x: 1}}, {multi: false}),
            ErrorCodes.InvalidOptions);
        // {key: -5} and {key: 5} fall on different shards for both hashed and ranged shard keys.
        assert.commandFailedWithCode(
            coll.update({$or: [{key: -5}, {key: 5}]}, {$set: {x: 1}}, {multi: false}),
            ErrorCodes.InvalidOptions);
    }

    // Make sure failed shard key or _id extraction doesn't affect the other
    assert.commandWorked(
        coll.update({'_id.x': ObjectId(), key: 1}, {$set: {x: 1}}, {multi: false}));
    assert.commandWorked(
        coll.update({_id: ObjectId(), 'key.x': 1}, {$set: {x: 1}}, {multi: false}));

    // Can unset shard key with op style update.
    assert.commandWorked(coll.insert({_id: 11, key: 1}));
    assert.commandWorked(sessionColl.update({_id: 11, key: 1}, {$unset: {key: 1}}));
    assert.docEq({_id: 11}, sessionColl.findOne({_id: 11}));

    // Can unset shard key with replacement style update.
    assert.commandWorked(coll.insert({_id: 12, key: 1}));
    assert.commandWorked(sessionColl.update({_id: 12, key: 1}, {_id: 12}));
    assert.docEq({_id: 12}, sessionColl.findOne({_id: 12}));

    // Can unset shard key with pipeline style update.
    assert.commandWorked(coll.insert({_id: 13, key: 1}));
    assert.commandWorked(sessionColl.update({_id: 13, key: 1}, [{$unset: "key"}, {$set: {x: 1}}]));
    assert.docEq({_id: 13, x: 1}, sessionColl.findOne({_id: 13}));

    // Can unset nested fields in the shard key.
    assert.commandWorked(coll.insert({_id: 14, key: {a: 1, b: 1}}));
    assert.commandWorked(sessionColl.update({_id: 14, key: {a: 1, b: 1}}, {$unset: {"key.a": 1}}));
    assert.docEq({_id: 14, key: {b: 1}}, sessionColl.findOne({_id: 14}));
}

// Run the above testcases twice, once with an ascending shard key and then again with a hashed
// shard key.
testShardKeys("update0", false);
testShardKeys("update1", true);

function testNestedShardKeys(collName, hashedKey) {
    const coll = db.getCollection(collName);
    const sessionColl = sessionDb.getCollection(collName);
    const keyPattern = hashedKey ? {"skey.skey": "hashed"} : {"skey.skey": 1};

    // Create an index on the shard key pattern. Then add a single document to the collection
    // so that it's not empty.
    coll.createIndex(keyPattern);
    coll.insert({_id: -1, skey: {skey: -999}});

    // Shard the collection. After sharding, the collection should have a single chunk that is
    // located on shard1 (the primary shard).
    s.adminCommand({shardCollection: dbName + "." + collName, key: keyPattern});

    // Remove all documents from the collection before executing the testcases below.
    coll.remove({});

    //
    // Verify full shard key path can be unset.
    //

    // Can unset shard key with op style update.
    assert.commandWorked(coll.insert({_id: 11, skey: {skey: 1}}));
    assert.commandWorked(sessionColl.update({_id: 11, "skey.skey": 1}, {$unset: {skey: 1}}));
    assert.docEq({_id: 11}, sessionColl.findOne({_id: 11}));

    // Can unset shard key with replacement style update.
    assert.commandWorked(coll.insert({_id: 12, skey: {skey: 1}}));
    assert.commandWorked(sessionColl.update({_id: 12, "skey.skey": 1}, {_id: 12}));
    assert.docEq({_id: 12}, sessionColl.findOne({_id: 12}));

    // Can unset shard key with pipeline style update.
    assert.commandWorked(coll.insert({_id: 13, skey: {skey: 1}}));
    assert.commandWorked(
        sessionColl.update({_id: 13, "skey.skey": 1}, [{$unset: "skey"}, {$set: {x: 1}}]));
    assert.docEq({_id: 13, x: 1}, sessionColl.findOne({_id: 13}));

    //
    // Verify each field in a nested shard key can be unset.
    //

    // For op-style.
    assert.commandWorked(coll.insert({_id: 14, skey: {skey: 1}}));
    assert.commandWorked(sessionColl.update({_id: 14, "skey.skey": 1}, {$unset: {"skey.skey": 1}}));
    assert.docEq({_id: 14, skey: {}}, sessionColl.findOne({_id: 14}));
    assert.commandWorked(sessionColl.update({_id: 14, skey: {}}, {$unset: {skey: 1}}));
    assert.docEq({_id: 14}, sessionColl.findOne({_id: 14}));

    // For replacement style.
    assert.commandWorked(coll.insert({_id: 15, skey: {skey: 1}}));
    assert.commandWorked(sessionColl.update({_id: 15, "skey.skey": 1}, {skey: 1}));
    assert.docEq({_id: 15, skey: 1}, sessionColl.findOne({_id: 15}));
    assert.commandWorked(sessionColl.update({_id: 15, skey: 1}, {$unset: {skey: 1}}));
    assert.docEq({_id: 15}, sessionColl.findOne({_id: 15}));

    // This can be used to make sure pipeline-based updates generate delta oplog entries.
    const largeStr = '*'.repeat(128);
    // For pipeline style.
    assert.commandWorked(coll.insert({_id: 16, skey: {skey: 1}, largeStr: largeStr}));
    assert.commandWorked(sessionColl.update({_id: 16, "skey.skey": 1}, [{$unset: "skey.skey"}]));
    assert.docEq({_id: 16, skey: {}, largeStr: largeStr}, sessionColl.findOne({_id: 16}));
    assert.commandWorked(sessionColl.update({_id: 16, skey: {}}, [{$unset: "skey"}]));
    assert.docEq({_id: 16, largeStr: largeStr}, sessionColl.findOne({_id: 16}));
}

// Run the testcases for nested shard keys twice, once with an ascending shard key and then again
// with a hashed shard key.
testNestedShardKeys("update_nested", false);
testNestedShardKeys("update_nested_hashed", true);

s.stop();
