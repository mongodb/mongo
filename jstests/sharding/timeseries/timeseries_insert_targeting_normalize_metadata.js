/**
 * Tests that when inserting into a sharded time-series collection, the targeting takes into account
 * metadata normalization.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const db = st.s0.getDB(jsTestName());
const coll = db.coll;
const bucketsColl = db.system.buckets.coll;

assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {m: 1}}));
assert.commandWorked(st.splitAt(bucketsColl.getFullName(), {meta: {a: 5, b: 5}}));
assert.commandWorked(
    st.moveChunk(bucketsColl.getFullName(), {meta: {a: 0, b: 0}}, st.shard0.shardName));
assert.commandWorked(
    st.moveChunk(bucketsColl.getFullName(), {meta: {a: 10, b: 10}}, st.shard1.shardName));

assert.commandWorked(
    coll.insert({_id: 0, t: ISODate("2023-01-01T12:00:00.000Z"), m: {a: 1, b: 1}}));
assert.commandWorked(
    coll.insert({_id: 1, t: ISODate("2023-01-01T12:00:01.000Z"), m: {b: 1, a: 1}}));

assert.eq(coll.find().itcount(), 2);

st.stop();
