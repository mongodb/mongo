/**
 * Tests that when inserting into a sharded time-series collection, the targeting takes into account
 * metadata normalization.
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const db = st.s0.getDB(jsTestName());
const coll = db.coll;
const timeFieldName = "time";
const metaFieldName = "metaField";
const rawMetaFieldName = "meta";

function testTargetingRespectsNormalizedMetadata(
    shardingKey, valueToSplitAt, chunk1, chunk2, measurementsToInsert) {
    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: shardingKey}));
    assert.commandWorked(
        st.splitAt(getTimeseriesCollForDDLOps(db, coll).getFullName(), valueToSplitAt));
    assert.commandWorked(st.moveChunk(
        getTimeseriesCollForDDLOps(db, coll).getFullName(), chunk1, st.shard0.shardName));
    assert.commandWorked(st.moveChunk(
        getTimeseriesCollForDDLOps(db, coll).getFullName(), chunk2, st.shard1.shardName));

    for (let i = 0; i < measurementsToInsert.length; i++) {
        assert.commandWorked(coll.insert(measurementsToInsert[i]));
    }
    // We should be able to find all the measurements that we inserted.
    assert.eq(coll.find().itcount(), measurementsToInsert.length);
}

testTargetingRespectsNormalizedMetadata(
    /*shardingKey=*/ {[metaFieldName]: 1},
    /*valueToSplitAt=*/ {[rawMetaFieldName]: {a: 5, b: 5}},
    /*chunk1=*/ {[rawMetaFieldName]: {a: 0, b: 0}},
    /*chunk2=*/ {[rawMetaFieldName]: {a: 10, b: 10}},
    /*measurements=*/
    [
        {[timeFieldName]: ISODate("2025-02-18T12:00:00.000Z"), [metaFieldName]: {a: 1, b: 1}},
        {[timeFieldName]: ISODate("2025-02-18T12:00:01.000Z"), [metaFieldName]: {b: 1, a: 1}}
    ]);

testTargetingRespectsNormalizedMetadata(
    /*shardingKey=*/ {[metaFieldName + ".nested"]: 1},
    /*valueToSplitAt=*/ {[rawMetaFieldName + ".nested"]: {a: 5, b: 5}},
    /*chunk1=*/ {[rawMetaFieldName + ".nested"]: {a: 0, b: 0}},
    /*chunk2=*/ {[rawMetaFieldName + ".nested"]: {a: 10, b: 10}},
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
