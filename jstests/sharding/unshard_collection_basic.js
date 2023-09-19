/**
 * Tests for basic functionality of the unshard collection feature.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagUnshardCollection
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
let shard = st.shard0;
let cmdObj = {unshardCollection: ns, toShard: st.shard0.shardName};
const topology = DiscoverTopology.findConnectedNodes(mongos);
const configsvr = new Mongo(topology.configsvr.nodes[0]);

// Fail if collection does not exist.
assert.commandFailedWithCode(mongos.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

// Implicit collection creation.
const coll = st.s.getDB(dbName)["collName"];
assert.commandWorked(coll.insert({oldKey: 1}));

// Fail if collection is unsharded.
let result = mongos.adminCommand(cmdObj);
assert.commandFailedWithCode(result, ErrorCodes.NamespaceNotSharded);
assert.eq(result.errmsg, "Namespace must be sharded to perform an unshardCollection command");

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(coll.createIndex({oldKey: 1}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

// Unshard collection should succeed with and without toShard option.
assert.commandWorked(mongos.adminCommand({unshardCollection: ns}));
assert.commandWorked(mongos.adminCommand(cmdObj));

// Fail if command called on shard.
assert.commandFailedWithCode(shard.adminCommand(cmdObj), ErrorCodes.CommandNotFound);

const metrics = configsvr.getDB('admin').serverStatus({}).shardingStatistics.unshardCollection;

assert.eq(metrics.countStarted, 2);
assert.eq(metrics.countSucceeded, 2);
assert.eq(metrics.countFailed, 0);
assert.eq(metrics.countCanceled, 0);

st.stop();
})();
