/**
 * Tests for basic functionality of the unshard collection feature.
 *
 * @tags: [
 *  requires_fcv_71,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection
 * ]
 */

(function() {
'use strict';

var st = new ShardingTest({mongos: 1, shards: 2});

const dbName = 'db';
const collName = 'foo';
const ns = dbName + '.' + collName;
let mongos = st.s0;
let shard = st.shard0;
let cmdObj = {unshardCollection: ns, toShard: st.shard0.shardName};

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
// TODO (SERVER-80265): Update once we support not passing in toShard.
assert.commandFailedWithCode(mongos.adminCommand({unshardCollection: ns}), 8018401);
assert.commandWorked(mongos.adminCommand(cmdObj));

// Fail if command called on shard.
assert.commandFailedWithCode(shard.adminCommand(cmdObj), ErrorCodes.CommandNotFound);

st.stop();
})();
