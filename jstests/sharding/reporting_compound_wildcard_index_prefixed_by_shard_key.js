/**
 *
 * Tests that the server reports the presence of compound wildcard indexes prefixed by the shard
 * key when attempting to fetch a shard key index.
 *
 * TODO (SERVER-112793): Remove this test once 9.0 branches out.
 *
 * @tags: [
 *   requires_fcv_82,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 3,
});

const shardKeyPattern = {
    skey: 1,
};

function checkLogAndServerStatusMetrics(shard, expectedLogCount, expectedServerStatusMetricCount) {
    checkLog.containsWithCount(
        shard, "Found a compound wildcard index prefixed by the shard key", 1);
    checkLog.checkContainsWithCountJson(
        shard,
        11279201,
        {
            nss: coll.getFullName(),
            index: {skey: 1, "a.$**": 1},
            indexName: "skey_1_a.$**_1",
            shardKey: shardKeyPattern
        },
        expectedLogCount,
    );
    assert.eq(
        expectedServerStatusMetricCount,
        shard.getDB("admin")
            .serverStatus()
            .shardingStatistics.countHitsOfCompoundWildcardIndexesWithShardKeyPrefix,
    );
}

const db = st.s.getDB(jsTestName());
const coll = db.getCollection("coll");
const primaryShard = st.shard0;

assert.commandWorked(
    st.s.adminCommand({enableSharding: db.getName(), primaryShard: primaryShard.shardName}));

coll.createIndex({skey: 1, "a.$**": 1});
coll.createIndex({skey: 1});
assert.eq(3, coll.getIndexes().length, tojson(coll.getIndexes()));

assert.commandWorked(
    st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKeyPattern}));
checkLogAndServerStatusMetrics(primaryShard, 1, 1);

assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {skey: 0}, to: st.shard1.shardName}));
checkLogAndServerStatusMetrics(primaryShard, 1, 2);
checkLogAndServerStatusMetrics(st.shard1, 1, 1);

assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {skey: 0}, to: st.shard2.shardName}));
checkLogAndServerStatusMetrics(st.shard1, 1, 2);
checkLogAndServerStatusMetrics(st.shard2, 1, 1);

st.stop();
