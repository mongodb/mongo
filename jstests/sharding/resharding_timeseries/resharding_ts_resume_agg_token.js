/**
 * Tests $_requestResumeToken in aggregate command on the raw timeseries buckets.
 *
 * @tags: [
 *  requires_fcv_80,
 * ]
 */

import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = "db";
const kCollName = "foo";
const ns = kDbName + "." + kCollName;
const sDB = st.s.getDB(kDbName);

assert.commandWorked(sDB.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));

const timeFieldName = "time";
const metaFieldName = "meta";

const timeseriesOptions = {
    timeField: timeFieldName,
    metaField: metaFieldName,
};

assert.commandWorked(
    st.s.adminCommand({
        shardCollection: ns,
        key: {"meta.x": 1},
        timeseries: timeseriesOptions,
    }),
);

const doc1 = {
    data: 1,
    time: new Date("2025-01-01T01:00:00Z"),
    meta: {x: 1, y: 5},
};
const doc2 = {
    data: 3,
    time: new Date("2025-01-01T01:00:01Z"),
    meta: {x: 2, y: 3},
};
const doc3 = {
    data: 3,
    time: new Date("2025-01-01T01:00:02Z"),
    meta: {x: 3, y: 2},
};
const doc4 = {
    data: 1,
    time: new Date("2025-01-01T01:00:03Z"),
    meta: {x: 4, y: 1},
};

// Insert some docs
assert.commandWorked(sDB.getCollection(kCollName).insertOne(doc1));
assert.commandWorked(sDB.getCollection(kCollName).insertOne(doc2));
assert.commandWorked(sDB.getCollection(kCollName).insertOne(doc3));
assert.commandWorked(sDB.getCollection(kCollName).insertOne(doc4));

const db = st.rs0.getPrimary().getDB(kDbName);
const coll = db.getCollection(kCollName);

jsTest.log("aggregate with $requestResumeToken should return PBRT with recordId and an initialSyncId.");
let res = assert.commandWorked(
    db.runCommand({
        aggregate: getTimeseriesCollForRawOps(db, coll).getName(),
        pipeline: [],
        $_requestResumeToken: true,
        hint: {$natural: 1},
        cursor: {batchSize: 1},
        ...getRawOperationSpec(db),
    }),
);
assert.hasFields(res.cursor, ["postBatchResumeToken"]);
assert.hasFields(res.cursor.postBatchResumeToken, ["$recordId"]);
assert.hasFields(res.cursor.postBatchResumeToken, ["$initialSyncId"]);

assert.eq(res.cursor.firstBatch[0].meta, doc1.meta);
const resumeToken = res.cursor.postBatchResumeToken;

res = assert.commandWorked(
    db.runCommand({
        aggregate: getTimeseriesCollForRawOps(db, coll).getName(),
        pipeline: [],
        $_requestResumeToken: true,
        hint: {$natural: 1},
        $_resumeAfter: resumeToken,
        cursor: {batchSize: 1},
        ...getRawOperationSpec(db),
    }),
);

assert.eq(res.cursor.firstBatch[0].meta, doc2.meta);

st.stop();
