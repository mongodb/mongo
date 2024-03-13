/**
 * Tests for basic functionality of the unshard collection feature.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagUnshardCollection,
 *  # TODO (SERVER-87812) Remove multiversion_incompatible tag
 *  multiversion_incompatible,
 *  assumes_balancer_off,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";

(function() {
'use strict';

var st = new ShardingTest({mongos: 1, shards: 2});

const dbName = 'db';
const collName = 'foo';
const ns = dbName + '.' + collName;
let mongos = st.s0;
let shard0 = st.shard0.shardName;
let shard1 = st.shard1.shardName;
let cmdObj = {unshardCollection: ns, toShard: shard1};

// Fail if unsharded collection.
assert.commandFailedWithCode(mongos.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0}));

let coll = mongos.getDB(dbName)[collName];
assert.commandWorked(coll.insert({oldKey: 50}));

// Fail if unsplittable tracked collection.
const unsplittableCollName = "foo_unsplittable"
const unsplittableCollNs = dbName + '.' + unsplittableCollName;
assert.commandWorked(
    st.s.getDB(dbName).runCommand({createUnsplittableCollection: unsplittableCollName}));
assert.commandFailedWithCode(mongos.adminCommand({unshardCollection: unsplittableCollNs}),
                             ErrorCodes.NamespaceNotSharded);

// Fail if unsharded collection.
const unshardedCollName = "foo_unsharded"
const unshardedCollNS = dbName + '.' + unshardedCollName;
assert.commandWorked(st.s.getDB(dbName).runCommand({create: unshardedCollName}));
assert.commandFailedWithCode(mongos.adminCommand({unshardCollection: unshardedCollNS}),
                             [ErrorCodes.NamespaceNotFound, ErrorCodes.NamespaceNotSharded]);

assert.commandWorked(coll.createIndex({oldKey: 1}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

assert.commandWorked(mongos.adminCommand({split: ns, middle: {oldKey: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: -1}, to: shard0}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: 10}, to: shard1}));

coll = mongos.getDB(dbName)[collName];
for (let i = -25; i < 25; ++i) {
    assert.commandWorked(coll.insert({oldKey: i}));
}

assert.eq(26, st.rs1.getPrimary().getCollection(ns).countDocuments({}));

// Unshard collection should succeed with toShard option.
assert.commandWorked(mongos.adminCommand({unshardCollection: ns, toShard: shard1}));

// Should have unsplittable set to true
let configDb = mongos.getDB('config');
let unshardedColl = configDb.collections.findOne({_id: ns});
assert.eq(unshardedColl.unsplittable, true);

assert.eq(51, st.rs1.getPrimary().getCollection(ns).countDocuments({}));
assert.eq(0, st.rs0.getPrimary().getCollection(ns).countDocuments({}));

let unshardedChunk = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
assert.eq(1, unshardedChunk.length);

// Fail since collection is unsharded now
assert.commandFailedWithCode(mongos.adminCommand({unshardCollection: ns}),
                             ErrorCodes.NamespaceNotSharded);

const newCollName = "foo1"
const newCollNs = dbName + '.' + newCollName
assert.commandWorked(mongos.adminCommand({shardCollection: newCollNs, key: {oldKey: 1}}));

assert.commandWorked(mongos.adminCommand({split: newCollNs, middle: {oldKey: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: newCollNs, find: {oldKey: -1}, to: shard0}));
assert.commandWorked(mongos.adminCommand({moveChunk: newCollNs, find: {oldKey: 10}, to: shard1}));

coll = mongos.getDB(dbName)[newCollName];
for (let i = -30; i < 30; ++i) {
    assert.commandWorked(coll.insert({oldKey: i}));
}

// Unshard collection should succeed without toShard option.
assert.commandWorked(mongos.adminCommand({unshardCollection: newCollNs}));

assert(st.rs1.getPrimary().getCollection(newCollNs).countDocuments({}) == 60 ||
       st.rs0.getPrimary().getCollection(newCollNs).countDocuments({}) == 60);

// Fail if command called on shard.
assert.commandFailedWithCode(st.shard0.adminCommand(cmdObj), ErrorCodes.CommandNotFound);

assert.commandWorked(mongos.adminCommand({shardCollection: newCollNs, key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({split: newCollNs, middle: {_id: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: newCollNs, find: {_id: -1}, to: shard0}));
assert.commandWorked(mongos.adminCommand({moveChunk: newCollNs, find: {_id: 10}, to: shard1}));

coll = mongos.getDB(dbName)[newCollName];
for (let i = -30; i < 30; ++i) {
    assert.commandWorked(coll.insert({_id: i}));
}

assert(st.rs0.getPrimary().getCollection(newCollNs).countDocuments({}) == 30)

// Unshard collection should succeed when collection's original shard key is _id.
assert.commandWorked(mongos.adminCommand({unshardCollection: newCollNs}));

assert(st.rs1.getPrimary().getCollection(newCollNs).countDocuments({}) == 120 ||
       st.rs0.getPrimary().getCollection(newCollNs).countDocuments({}) == 120);

const metrics = st.config0.getDB('admin').serverStatus({}).shardingStatistics.unshardCollection;

assert.eq(metrics.countStarted, 3);
assert.eq(metrics.countSucceeded, 3);
assert.eq(metrics.countFailed, 0);
assert.eq(metrics.countCanceled, 0);

st.stop();
})();
