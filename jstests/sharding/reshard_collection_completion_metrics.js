/**
 * Verifies the additional collection metrics in the "resharding complete" log after resharding
 * operation succeeds.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  multiversion_incompatible,
 *  assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

(function() {
'use strict';

var st = new ShardingTest({mongos: 1, shards: 2});

const dbName = 'db';
const collName = "foo";
const ns = dbName + "." + collName;
let mongos = st.s0;
let shard0 = st.shard0.shardName;
let shard1 = st.shard1.shardName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

assert.commandWorked(mongos.adminCommand({split: ns, middle: {oldKey: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: -1}, to: shard0}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: 10}, to: shard1}));

const coll = mongos.getDB(dbName)[collName];
for (let i = -50; i < 50; ++i) {
    assert.commandWorked(coll.insert({oldKey: i}));
}
assert.commandWorked(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    shardDistribution: [
        {shard: shard0, min: {newKey: MinKey}, max: {newKey: -1}},
        {shard: shard0, min: {newKey: -1}, max: {newKey: 1}},
        {shard: shard1, min: {newKey: 1}, max: {newKey: MaxKey}}
    ]
}));

// Determine the collection stats.
let res = assert.commandWorked(coll.stats());
const averageDocSize = res.avgObjSize;
const count = res.count;
const indexes = res.nindexes;

let logMsg = JSON.parse(checkLog.containsLog(st.config0, 7763800));
assert(logMsg);

let metrics = logMsg.attr.info.statistics;
assert.gt(metrics.operationDuration, 0);
assert.eq(metrics.numberOfTotalDocuments, count);
assert.eq(metrics.averageDocSize, averageDocSize);
assert.eq(metrics.numberOfIndexes, indexes);
assert.eq(metrics.numberOfSourceShards, 2);
assert.eq(metrics.numberOfDestinationShards, 2);

st.stop();
})();
