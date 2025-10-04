// The validate command should work in the following scenarios on a sharded environment with 3 or
// more shards:
//
// 1. Collection in an unsharded DB.
// 2. Sharded collection.
// 3. Sharded collection with chunks on two shards while the collection's DB exists on 3 or more
//    shards. We enforce the latter condition by creating a dummy collection within the same
//    database and splitting it across the shards. See SERVER-22588 for details.
// 4. The previous scenario, but with validation legitimately failing on one of the shards.

import {ShardingTest} from "jstests/libs/shardingtest.js";

const NUM_SHARDS = 3;
assert(NUM_SHARDS >= 3);

let st = new ShardingTest({shards: NUM_SHARDS});
let s = st.s;
let testDb = st.getDB("test");
assert.commandWorked(s.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));

function setup() {
    assert.commandWorked(testDb.test.insert({_id: 0}));
    assert.commandWorked(testDb.test.insert({_id: 1}));

    assert.commandWorked(testDb.dummy.insert({_id: 0}));
    assert.commandWorked(testDb.dummy.insert({_id: 1}));
    assert.commandWorked(testDb.dummy.insert({_id: 2}));
}

function validate(valid) {
    let res = testDb.runCommand({validate: "test"});
    assert.commandWorked(res);
    assert.eq(res.valid, valid, tojson(res));
}

function setFailValidateFailPointOnShard(enabled, shard) {
    let mode;
    if (enabled) {
        mode = "alwaysOn";
    } else {
        mode = "off";
    }

    let res = shard.adminCommand({configureFailPoint: "validateCmdCollectionNotValid", mode: mode});
    assert.commandWorked(res);
}

setup();

// 1. Collection in an unsharded DB.
validate(true);

// 2. Sharded collection in a DB.
assert.commandWorked(s.adminCommand({shardCollection: "test.test", key: {_id: 1}}));
assert.commandWorked(s.adminCommand({shardCollection: "test.dummy", key: {_id: 1}}));
validate(true);

// 3. Sharded collection with chunks on two shards.
assert.commandWorked(s.adminCommand({split: "test.test", middle: {_id: 1}}));
assert.commandWorked(testDb.adminCommand({moveChunk: "test.test", find: {_id: 1}, to: st.shard1.shardName}));
// We move the dummy database to NUM_SHARDS shards so that testDb will exist on all NUM_SHARDS
// shards but the testDb.test collection will only exist on the first two shards. Prior to
// SERVER-22588, this scenario would cause validation to fail.
assert.commandWorked(s.adminCommand({split: "test.dummy", middle: {_id: 1}}));
assert.commandWorked(s.adminCommand({split: "test.dummy", middle: {_id: 2}}));
assert.commandWorked(testDb.adminCommand({moveChunk: "test.dummy", find: {_id: 1}, to: st.shard1.shardName}));
assert.commandWorked(testDb.adminCommand({moveChunk: "test.dummy", find: {_id: 2}, to: st.shard2.shardName}));
assert.eq(st.onNumShards("test", "test"), 2);
assert.eq(st.onNumShards("test", "dummy"), NUM_SHARDS);
validate(true);

// 4. Fail validation on one of the shards.
let primaryShard = st.getPrimaryShard("test");
setFailValidateFailPointOnShard(true, primaryShard);
validate(false);
setFailValidateFailPointOnShard(false, primaryShard);

st.stop();
