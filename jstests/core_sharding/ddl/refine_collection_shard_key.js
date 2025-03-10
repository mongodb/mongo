/**
 * Basic tests for refineCollectionShardKey.
 * @tags: [
 *  assumes_balancer_off,
 *  does_not_support_stepdowns,
 *  uses_transactions,
 *  # This test performs explicit calls to shardCollection
 *  assumes_unsharded_collection,
 * ]
 */
import {
    compareBoundaries,
    integrationTests,
    shardKeyValidationTests,
    simpleValidationTests,
    uniquePropertyTests,
} from "jstests/sharding/libs/refine_collection_shard_key_common.js";
import {getShardNames} from "jstests/sharding/libs/sharding_util.js";

const kDbName = db.getName();
const kCollName = jsTestName();
const kNsName = kDbName + "." + kCollName;
const shardNames = getShardNames(db);
const mongos = db.getMongo();

// 1. Assume oldKeyDoc = {a: 1, b: 1} when validating operations before
//    'refineCollectionShardKey'.
// 2. Assume newKeyDoc = {a: 1, b: 1, c: 1, d: 1} when validating operations after
//    'refineCollectionShardKey'.

function setupCRUDBeforeRefine() {
    const session = mongos.startSession({retryWrites: true});
    const sessionDB = session.getDatabase(kDbName);

    // The documents below will be read after refineCollectionShardKey to verify data integrity.
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 5, b: 5, c: 5, d: 5}));
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 10, b: 10, c: 10, d: 10}));
}

function validateCRUDAfterRefine() {
    const session = mongos.startSession({retryWrites: true});
    const sessionDB = session.getDatabase(kDbName);

    // Verify that documents inserted before refineCollectionShardKey have not been corrupted.
    assert.eq([{a: 5, b: 5, c: 5, d: 5}],
              sessionDB.getCollection(kCollName).find({a: 5}, {_id: 0}).toArray());
    assert.eq([{a: 10, b: 10, c: 10, d: 10}],
              sessionDB.getCollection(kCollName).find({a: 10}, {_id: 0}).toArray());

    // A write with the incomplete shard key is treated as if the missing values are null.
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 1, b: 1}));
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: -1, b: -1}));

    assert.neq(null, sessionDB.getCollection(kCollName).findOne({a: 1, b: 1, c: null, d: null}));
    assert.neq(null, sessionDB.getCollection(kCollName).findOne({a: -1, b: -1, c: null, d: null}));

    // Full shard key writes work properly.
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 1, b: 1, c: 1, d: 1}));
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: -1, b: -1, c: -1, d: -1}));

    // This enables the feature allows writes to omit the shard key in their queries.
    assert.commandWorked(
        sessionDB.getCollection(kCollName).update({a: 1, b: 1, c: 1}, {$set: {x: 2}}));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).update({a: 1, b: 1, c: 1, d: 1}, {$set: {b: 2}}));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).update({a: -1, b: -1, c: -1, d: -1}, {$set: {b: 4}}));

    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).x);
    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
    assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);

    // Versioned reads against secondaries should work as expected.
    mongos.setReadPref("secondaryPreferred");
    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).x);
    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
    assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);
    mongos.setReadPref(null);
}

simpleValidationTests(mongos, kDbName);
shardKeyValidationTests(mongos, kDbName);
uniquePropertyTests(mongos, kDbName);

if (shardNames.length >= 2) {
    integrationTests(mongos, kDbName, shardNames[0], shardNames[1]);
} else {
    integrationTests(mongos, kDbName, shardNames[0]);
}

const oldKeyDoc = {
    a: 1,
    b: 1
};
const newKeyDoc = {
    a: 1,
    b: 1,
    c: 1,
    d: 1
};

assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: oldKeyDoc}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

// CRUD operations before and after refineCollectionShardKey should work as expected.
setupCRUDBeforeRefine();
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateCRUDAfterRefine();

assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));

//
// Verify the chunk boundaries are the same for a collection sharded to a certain shard key and
// a collection refined to that same shard key.
//

// For a shard key without nested fields.
(() => {
    const shardedNs = kDbName + ".shardedColl";
    const refinedNs = kDbName + ".refinedColl";

    assert.commandWorked(
        mongos.adminCommand({shardCollection: shardedNs, key: {a: 1, b: 1, c: 1}}));
    assert.commandWorked(
        mongos.adminCommand({split: shardedNs, middle: {a: 0, b: MinKey, c: MinKey}}));
    assert.commandWorked(
        mongos.adminCommand({split: shardedNs, middle: {a: 10, b: MinKey, c: MinKey}}));

    assert.commandWorked(mongos.adminCommand({shardCollection: refinedNs, key: {a: 1}}));
    assert.commandWorked(mongos.adminCommand({split: refinedNs, middle: {a: 0}}));
    assert.commandWorked(mongos.adminCommand({split: refinedNs, middle: {a: 10}}));

    assert.commandWorked(mongos.getCollection(refinedNs).createIndex({a: 1, b: 1, c: 1}));
    assert.commandWorked(
        mongos.adminCommand({refineCollectionShardKey: refinedNs, key: {a: 1, b: 1, c: 1}}));

    compareBoundaries(mongos, shardedNs, refinedNs);
})();

// For a shard key with nested fields.
(() => {
    const shardedNs = kDbName + ".nestedShardedColl";
    const refinedNs = kDbName + ".nestedRefinedColl";

    assert.commandWorked(
        mongos.adminCommand({shardCollection: shardedNs, key: {"a.b": 1, "c.d.e": 1, f: 1}}));

    assert.commandWorked(
        mongos.adminCommand({split: shardedNs, middle: {"a.b": 0, "c.d.e": MinKey, f: MinKey}}));
    assert.commandWorked(
        mongos.adminCommand({split: shardedNs, middle: {"a.b": 10, "c.d.e": MinKey, f: MinKey}}));

    assert.commandWorked(mongos.adminCommand({shardCollection: refinedNs, key: {"a.b": 1}}));
    assert.commandWorked(mongos.adminCommand({split: refinedNs, middle: {"a.b": 0}}));
    assert.commandWorked(mongos.adminCommand({split: refinedNs, middle: {"a.b": 10}}));

    assert.commandWorked(mongos.getCollection(refinedNs).createIndex({"a.b": 1, "c.d.e": 1, f: 1}));
    assert.commandWorked(mongos.adminCommand(
        {refineCollectionShardKey: refinedNs, key: {"a.b": 1, "c.d.e": 1, f: 1}}));

    compareBoundaries(mongos, shardedNs, refinedNs);
})();
