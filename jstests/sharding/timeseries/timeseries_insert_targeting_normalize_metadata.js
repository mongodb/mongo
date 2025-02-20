/**
 * Tests that when inserting into a sharded time-series collection, the targeting takes into account
 * metadata normalization.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const db = st.s0.getDB(jsTestName());
const coll = db.coll;
const bucketsColl = db.system.buckets.coll;
const timeFieldName = "time";
const metaFieldName = "metaField";
const bucketsMetaFieldName = "meta";

function testTargetingRespectsNormalizedMetadata(
    shardingKey, valueToSplitAt, chunk1, chunk2, measurementsToInsert) {
    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: shardingKey}));
    assert.commandWorked(st.splitAt(bucketsColl.getFullName(), valueToSplitAt));
    assert.commandWorked(st.moveChunk(bucketsColl.getFullName(), chunk1, st.shard0.shardName));
    assert.commandWorked(st.moveChunk(bucketsColl.getFullName(), chunk2, st.shard1.shardName));

    for (let i = 0; i < measurementsToInsert.length; i++) {
        assert.commandWorked(coll.insert(measurementsToInsert[i]));
    }
    // We should be able to find all the measurements that we inserted.
    assert.eq(coll.find().itcount(), measurementsToInsert.length);
}

testTargetingRespectsNormalizedMetadata(
    /*shardingKey=*/ {[metaFieldName]: 1},
    /*valueToSplitAt=*/ {[bucketsMetaFieldName]: {a: 5, b: 5}},
    /*chunk1=*/ {[bucketsMetaFieldName]: {a: 0, b: 0}},
    /*chunk2=*/ {[bucketsMetaFieldName]: {a: 10, b: 10}},
    /*measurements=*/
    [
        {[timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"), [metaFieldName]: {a: 1, b: 1}},
        {[timeFieldName]: ISODate("2025-02-18T12:00:01.000Z"), [metaFieldName]: {b: 1, a: 1}}
    ]);

testTargetingRespectsNormalizedMetadata(
    /*shardingKey=*/ {[metaFieldName + ".nested"]: 1},
    /*valueToSplitAt=*/ {[bucketsMetaFieldName + ".nested"]: {a: 5, b: 5}},
    /*chunk1=*/ {[bucketsMetaFieldName + ".nested"]: {a: 0, b: 0}},
    /*chunk2=*/ {[bucketsMetaFieldName + ".nested"]: {a: 10, b: 10}},
    /*measurements=*/
    [
        {
            [timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"),
            [metaFieldName]: {nested: {a: 1, b: 1}}
        },
        {
            [timeFieldName]: ISODate("2025-02-18T12:00:01.000Z"),
            [metaFieldName]: {nested: {b: 1, a: 1}}
        }
    ]);

st.stop();
