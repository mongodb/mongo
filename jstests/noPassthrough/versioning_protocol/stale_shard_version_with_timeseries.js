/**
 * This test verifies the behavior of a stale router when interacting with a collection that was
 * dropped and re-created as a time-series collection. It ensures that the stale router can still
 * read documents from the re-created time-series collection.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, mongos: 2});
const dbName = "test";
const collName = "coll";
const ns = dbName + "." + collName;
const bucketNs = dbName + ".system.buckets." + collName;

const dbStaleRouter = st.s0.getDB(dbName);
const dbOtherRouter = st.s1.getDB(dbName);

// Force the db primary shard to be shard0.
assert.commandWorked(
    dbOtherRouter.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Create a sharded collection with chunks only in shard1.
assert.commandWorked(dbOtherRouter.adminCommand({shardCollection: ns, key: {x: 1}}));

assert.commandWorked(dbOtherRouter.adminCommand(
    {moveRange: ns, toShard: st.shard1.shardName, min: {x: MinKey}, max: {x: MaxKey}}));

// Both routers get a notion of the normal collection placed in shard1.
assert.commandWorked(dbOtherRouter[collName].insert({x: 1}));
assert.commandWorked(dbStaleRouter[collName].insert({x: 2}));

dbOtherRouter[collName].drop();

// Re-create the same collection as timeseries with chunks only in shard1 and some data.
assert.commandWorked(dbOtherRouter.adminCommand(
    {shardCollection: ns, key: {time: 1}, timeseries: {timeField: "time"}}));
assert.commandWorked(dbOtherRouter.adminCommand({
    moveRange: bucketNs,
    toShard: st.shard1.shardName,
    min: {"control.min.time": MinKey},
    max: {"control.min.time": MaxKey}
}));
const time = ISODate();
assert.commandWorked(dbOtherRouter[collName].insert({time: time}));

// Verify that the stale router reads the document previously inserted.
const res = dbStaleRouter[collName].find().toArray();
assert.eq(1, res.length);
assert.eq(time, res[0].time);

st.stop();
