/**
 * Test that we correctly use the index created when a time series collection is sharded.
 *
 * @tags: [
 *   requires_fcv_51,
 *   requires_sharding,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

Random.setRandomSeed();

const dbName = "testDB";
const collName = jsTestName();
const timeField = "t";
const metaField = "m";

const st = new ShardingTest({shards: 2});
const sDB = st.s.getDB(dbName);
assert.commandWorked(sDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const coll = sDB.getCollection(collName);

// Shard time-series collection.
const shardKey = {
    [timeField]: 1,
};
assert.commandWorked(
    sDB.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
        timeseries: {timeField, metaField, granularity: "hours"},
    }),
);

// Split the chunks.
const splitPoint = {
    [`control.min.${timeField}`]: new Date(50 * 1000),
};
assert.commandWorked(
    sDB.adminCommand({split: getTimeseriesCollForDDLOps(sDB, coll).getFullName(), middle: splitPoint}),
);

// // Move one of the chunks into the second shard.
const primaryShard = st.getPrimaryShard(dbName);
const otherShard = st.getOther(primaryShard);
assert.commandWorked(
    sDB.adminCommand({
        movechunk: getTimeseriesCollForDDLOps(sDB, coll).getFullName(),
        find: splitPoint,
        to: otherShard.name,
        _waitForDelete: true,
    }),
);

const hasInternalBoundedSort = (explain) => {
    for (const shardName in explain.shards) {
        const pipeline = explain.shards[shardName].stages;
        if (!pipeline.some((stage) => stage.hasOwnProperty("$_internalBoundedSort"))) {
            return false;
        }
    }
    return true;
};

const assertAccessPath = (pipeline, hint, accessPath, direction) => {
    const options = hint ? {hint: hint} : {};
    const explain = coll.explain().aggregate(pipeline, options);
    assert(hasInternalBoundedSort(explain));

    const paths = getAggPlanStages(explain, accessPath);
    for (const path of paths) {
        assert.eq(path.stage, accessPath);
        assert.eq(path.direction, direction > 0 ? "forward" : "backward");
    }
};

const assertNoRewrite = (pipeline) => {
    const explain = coll.explain().aggregate(pipeline);
    assert(!hasInternalBoundedSort(explain));
};

for (let i = 0; i < 100; i++) {
    assert.commandWorked(sDB.getCollection(collName).insert({t: new Date(i * 1000), m: i % 4, k: i}));
}

// Ensure that each shard owns one chunk.
const counts = st.chunkCounts(getTimeseriesCollForDDLOps(sDB, coll).getName(), dbName);
assert.eq(1, counts[primaryShard.shardName], counts);
assert.eq(1, counts[otherShard.shardName], counts);

assert.eq(coll.count(), 100);
assert.eq(getTimeseriesCollForRawOps(sDB, coll).count({}, getRawOperationSpec(sDB)), 4);

// The {meta: 1, time: 1} index gets built by default on the time-series collection.
assert.eq(coll.getIndexes().length, 2);

const indexName = "t_1";
assert.eq(coll.getIndexes()[1].name, indexName);

const forwardSort = {
    $sort: {t: 1},
};
const backwardSort = {
    $sort: {t: -1},
};
// One match before the split, one after the split.
for (const matchDate of [new Date(25 * 1000), new Date(75 * 1000)]) {
    const match = {$match: {t: matchDate}};
    assertAccessPath([match, forwardSort], null, "IXSCAN", 1);
    assertAccessPath([match, backwardSort], null, "IXSCAN", -1);
    assertNoRewrite([match, {$sort: {t: -1, m: 1}}]);
    assertNoRewrite([match, {$sort: {t: 1, m: 1}}]);
}
const kMatch = {
    $match: {k: 1},
};
assertAccessPath([forwardSort], null, "COLLSCAN", 1);
assertAccessPath([backwardSort], null, "COLLSCAN", -1);
assertAccessPath([kMatch, forwardSort], null, "COLLSCAN", 1);
assertAccessPath([kMatch, backwardSort], null, "COLLSCAN", -1);
assertAccessPath([forwardSort], {$natural: -1}, "COLLSCAN", 1);
assertAccessPath([backwardSort], {$natural: 1}, "COLLSCAN", -1);

st.stop();
