/**
 * Tests $_requestResumeToken in aggregate command on timeseries buckets collection.
 *
 * @tags: [
 *  requires_fcv_80,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'db';
const kCollName = 'foo';
const ns = kDbName + '.' + kCollName;
const sDB = st.s.getDB(kDbName);

assert.commandWorked(
    sDB.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));

const timeFieldName = 'time';
const metaFieldName = 'meta';

const timeseriesOptions = {
    timeField: timeFieldName,
    metaField: metaFieldName,
};

assert.commandWorked(st.s.adminCommand({
    shardCollection: ns,
    key: {'meta.x': 1},
    timeseries: timeseriesOptions,
}));

const kBucketCollName = "system.buckets.foo";

const doc1 = {
    data: 1,
    time: new Date(),
    meta: {x: 1, y: 5}
};
const doc2 = {
    data: 3,
    time: new Date(),
    meta: {x: 2, y: 3}
};
// Insert some docs
assert.commandWorked(sDB.getCollection(kCollName).insert([
    doc1,
    doc2,
    {data: 3, time: new Date(), meta: {x: 3, y: 2}},
    {data: 1, time: new Date(), meta: {x: 4, y: 1}}
]));

const db = st.rs0.getPrimary().getDB(kDbName);
jsTest.log(
    "aggregate with $requestResumeToken should return PBRT with recordId and an initialSyncId.");
let res = assert.commandWorked(db.runCommand({
    aggregate: kBucketCollName,
    pipeline: [],
    $_requestResumeToken: true,
    hint: {$natural: 1},
    cursor: {batchSize: 1}
}));
assert.hasFields(res.cursor, ["postBatchResumeToken"]);
assert.hasFields(res.cursor.postBatchResumeToken, ["$recordId"]);
assert.hasFields(res.cursor.postBatchResumeToken, ["$initialSyncId"]);
assert.eq(res.cursor.firstBatch[0].meta, doc1.meta);
const resumeToken = res.cursor.postBatchResumeToken;

res = assert.commandWorked(db.runCommand({
    aggregate: kBucketCollName,
    pipeline: [],
    $_requestResumeToken: true,
    hint: {$natural: 1},
    $_resumeAfter: resumeToken,
    cursor: {batchSize: 1}
}));
assert.eq(res.cursor.firstBatch[0].meta, doc2.meta);

st.stop();
