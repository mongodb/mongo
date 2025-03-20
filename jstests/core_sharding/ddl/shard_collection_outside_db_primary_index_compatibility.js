/**
 * Tests that sharding an collection which has been placed outside of the DB primary checks for
 * incompatible indexes (e.g. unique indexes which are not a prefix of a shard key) appropriately.
 * Regression test for SERVER-100844, where stale indexes remaining in the DB primary from previous
 * operations prevented sharding.
 *
 * @tags: [
 *   # Requires support for moveCollection / unshardCollection
 *   requires_fcv_80,
 *   # Commands based on reshardCollection require a custom value for the
 *   # minSnapshotHistoryWindowInSeconds server parameter, which is reset when a node is killed.
 *   does_not_support_stepdowns,
 *   # This test performs explicit calls to shardCollection
 *   assumes_unsharded_collection,
 *   # Requires placing a collection outside the DB primary.
 *   requires_2_or_more_shards,
 *   # Requires a deterministic placement for the collection.
 *   assumes_balancer_off,
 * ]
 */

import {getRandomShardName} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
const primaryShard = db.getDatabasePrimaryShardId();
const anotherShard = getRandomShardName(db, [primaryShard]);

let testId = 0;

// Sharding a collection moved to a non primary shard that has an incompatible index fails
{
    const coll = db.getCollection(jsTestName() + "_" + testId++);
    assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
    assert.commandWorked(
        db.adminCommand({moveCollection: coll.getFullName(), toShard: anotherShard}));
    assert.commandFailedWithCode(
        db.adminCommand({shardCollection: coll.getFullName(), key: {sk: 1}}),
        ErrorCodes.InvalidOptions);
}

// Sharding a collection moved to a non primary shard works when dropping the incompatible index
{
    const coll = db.getCollection(jsTestName() + "_" + testId++);
    assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
    assert.commandWorked(
        db.adminCommand({moveCollection: coll.getFullName(), toShard: anotherShard}));
    assert.commandWorked(coll.dropIndex({a: 1}));
    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {sk: 1}}));
}

// Sharding a collection after unsharding it to a non primary shard works when dropping the
// incompatible index
{
    const coll = db.getCollection(jsTestName() + "_" + testId++);
    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {sk1: 1}}));
    assert.commandWorked(coll.createIndex({sk1: 1, a: 1}, {unique: true}));
    assert.commandWorked(
        db.adminCommand({unshardCollection: coll.getFullName(), toShard: anotherShard}));
    assert.commandWorked(coll.dropIndex({sk1: 1, a: 1}));
    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {sk2: 1}}));
}

// Sharding a collection moved to a non primary shard works when dropping the incompatible index
// after a previous sharding attempt has already failed due to that incompatible index
{
    const coll = db.getCollection(jsTestName() + "_" + testId++);
    assert.commandWorked(db.runCommand({create: coll.getName()}));
    assert.commandWorked(
        db.adminCommand({moveCollection: coll.getFullName(), toShard: anotherShard}));
    assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
    assert.commandFailedWithCode(
        db.adminCommand({shardCollection: coll.getFullName(), key: {sk: 1}}),
        ErrorCodes.InvalidOptions);
    assert.commandWorked(coll.dropIndex({a: 1}));
    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {sk: 1}}));
}
