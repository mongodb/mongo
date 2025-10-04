/**
 * Tests for basic functionality of the move collection feature. Also tests reshardCollection on
 * unsplittible collection.
 *
 * @tags: [
 *  requires_fcv_80,
 *  requires_collstats,
 *  featureFlagMoveCollection,
 *  assumes_balancer_off,
 *  # Stepdown test coverage is already provided by the resharding FSM suites.
 *  does_not_support_stepdowns,
 *  # This test performs explicit calls to shardCollection
 *  assumes_unsharded_collection,
 * ]
 */

import {getPrimaryShardNameForDB, getShards} from "jstests/sharding/libs/sharding_util.js";

const shards = getShards(db);
if (shards.length < 2) {
    jsTestLog("This test requires at least two shards.");
    quit();
}

const collName = jsTestName();
const dbName = db.getName();
const ns = dbName + "." + collName;

let shard0 = shards[0];
let shard1 = shards[1];

let shardName0 = shard0._id;
let shardName1 = shard1._id;

let cmdObj = {moveCollection: ns, toShard: shardName0};

jsTestLog("Fail if collection is not tracked.");
assert.commandFailedWithCode(db.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

assert.commandWorked(db.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

jsTestLog("Fail if collection is sharded.");
assert.commandFailedWithCode(db.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

const unsplittableCollName = "foo_unsplittable";
const unsplittableCollNs = dbName + "." + unsplittableCollName;
assert.commandWorked(db.runCommand({create: unsplittableCollName}));

const coll = db.getCollection(unsplittableCollName);
const stats = coll.stats();
assert(stats.sharded != true);

const numDocuments = 1000;
for (let i = -numDocuments / 2; i < numDocuments / 2; ++i) {
    assert.commandWorked(coll.insert({oldKey: i}));
}

jsTestLog("Fail if missing required field toShard.");
assert.commandFailedWithCode(db.adminCommand({moveCollection: unsplittableCollNs}), ErrorCodes.IDLFailedToParse);

jsTestLog("Directly calling reshardCollection on the unsplittable collection should fail.");
assert.commandFailedWithCode(
    db.adminCommand({
        reshardCollection: unsplittableCollNs,
        key: {_id: 1},
        forceRedistribution: true,
        shardDistribution: [{shard: shardName1, min: {_id: MinKey}, max: {_id: MaxKey}}],
    }),
    [ErrorCodes.NamespaceNotSharded, ErrorCodes.NamespaceNotFound],
);

// Should fail to move to a shard with url instead of shard name
assert.commandFailedWithCode(
    db.adminCommand({moveCollection: unsplittableCollNs, toShard: shard0.host}),
    ErrorCodes.ShardNotFound,
);

const configDb = db.getSiblingDB("config");
const primaryShard = getPrimaryShardNameForDB(db);
const nonPrimaryShard = shardName0 == primaryShard ? shardName1 : shardName0;

jsTestLog("Move to non-primary shard (" + nonPrimaryShard + ")");
assert.commandWorked(db.adminCommand({moveCollection: unsplittableCollNs, toShard: nonPrimaryShard}));

// Should have unsplittable set to true
let unshardedColl = configDb.collections.findOne({_id: unsplittableCollNs});

assert.eq(unshardedColl.unsplittable, true);
let unshardedChunk = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
assert.eq(1, unshardedChunk.length);

// find on collection to confirm that all documents still exist
assert.eq(coll.find({}).itcount(), numDocuments);

jsTestLog("Move to primary shard (" + primaryShard + ")");
assert.commandWorked(db.adminCommand({moveCollection: unsplittableCollNs, toShard: primaryShard}));

// Should have unsplittable set to true
unshardedColl = configDb.collections.findOne({_id: unsplittableCollNs});
assert.eq(unshardedColl.unsplittable, true);
unshardedChunk = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
assert.eq(1, unshardedChunk.length);

// find on collection to confirm that all documents still exist
assert.eq(coll.find({}).itcount(), numDocuments);

coll.drop();
