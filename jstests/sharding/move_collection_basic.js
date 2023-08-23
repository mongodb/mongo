/**
 * Tests for basic functionality of the move collection feature.
 *
 * @tags: [
 *  require_fcv_71,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

(function() {
'use strict';

var st = new ShardingTest({mongos: 1, shards: 2});

const dbName = 'db';
const collName = 'foo';
const ns = dbName + '.' + collName;
let mongos = st.s0;
let shard = st.shard0;
let cmdObj = {moveCollection: ns, toShard: st.shard0.shardName};

if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements") ||
    !FeatureFlagUtil.isEnabled(mongos, "MoveCollection")) {
    jsTestLog(
        "Skipping test since featureFlagReshardingImprovements or featureFlagMoveCollection is not enabled");
    return;
}

// Fail if sharding is disabled.
assert.commandFailedWithCode(mongos.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

// Fail if collection is unsharded.
// TODO(SERVER-80156): update test case to succeed on unsharded collections
assert.commandFailedWithCode(mongos.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

// Fail if missing required field toShard.
assert.commandFailedWithCode(mongos.adminCommand({moveCollection: ns}), 40414);

// Succeed if command called on mongos.
assert.commandWorked(mongos.adminCommand(cmdObj));

// Fail if command called on shard.
assert.commandFailedWithCode(shard.adminCommand(cmdObj), ErrorCodes.CommandNotFound);

st.stop();
})();
