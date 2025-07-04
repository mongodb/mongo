/**
 * Tests the basic indexes operations on sharded time-series collection.
 * patterns.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {
    createRawTimeseriesIndex,
    getRawOperationSpec,
    getTimeseriesCollForRawOps
} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

Random.setRandomSeed();

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'time';
const metaField = 'hostid';

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

// Helpers.
let currentId = 0;
function generateId() {
    return currentId++;
}

function generateDoc(time, metaValue) {
    return TimeseriesTest.generateHosts(1).map((host, index) => Object.assign({}, host, {
        _id: generateId(),
        [metaField]: metaValue,
        [timeField]: ISODate(time),
    }))[0];
}

// Shard key on just the time field.
(function timeShardKey() {
    const mongosDB = st.s.getDB(dbName);
    assert.commandWorked(
        mongosDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    // Shard time-series collection.
    const shardKey = {[timeField]: 1};
    assert.commandWorked(mongosDB.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
        timeseries: {timeField, metaField, granularity: "hours"},
    }));

    const coll = mongosDB.getCollection(collName);

    // Split the chunks such that primary shard has chunk: [MinKey, 2020-01-01) and other shard has
    // chunk [2020-01-01, MaxKey].
    let splitPoint = {[`control.min.${timeField}`]: ISODate(`2020-01-01`)};
    assert.commandWorked(mongosDB.adminCommand(
        {split: getTimeseriesCollForDDLOps(mongosDB, coll).getFullName(), middle: splitPoint}));

    // Move one of the chunks into the second shard.
    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(mongosDB.adminCommand({
        movechunk: getTimeseriesCollForDDLOps(mongosDB, coll).getFullName(),
        find: splitPoint,
        to: otherShard.name,
        _waitForDelete: true,
    }));

    // Ensure that each shard owns one chunk.
    const counts = st.chunkCounts(collName, dbName);
    assert.eq(1, counts[primaryShard.shardName], counts);
    assert.eq(1, counts[otherShard.shardName], counts);

    // The {meta: 1, time: 1} index gets built by default on the time-series collection.
    let extraIndexes = [{[metaField]: 1, [timeField]: 1}];
    let extraBucketIndexes = [{"meta": 1, "control.min.time": 1, "control.max.time": 1}];

    for (let i = 0; i < 2; i++) {
        assert.commandWorked(coll.insert(generateDoc("2019-11-11", -1)));
        assert.commandWorked(coll.insert(generateDoc("2019-12-31", -1)));
        assert.commandWorked(coll.insert(generateDoc("2020-01-21", i)));
        assert.commandWorked(coll.insert(generateDoc("2020-11-31", 1)));
    }

    assert.commandWorked(coll.createIndex({[metaField]: 1}));

    let plan = coll.find({[metaField]: 0}).explain();
    assert.eq(getAggPlanStages(plan, "IXSCAN").length, 2, plan);

    const subField1 = `${metaField}.subField1`;
    const subField2 = `${metaField}.subField2`;

    assert.commandWorked(coll.createIndex({[subField1]: 1}));
    assert.commandWorked(coll.createIndex({[subField2]: 1}));

    const listIndexesOutput = assert.commandWorked(coll.runCommand({listIndexes: coll.getName()}));
    assert.eq(coll.getFullName(), listIndexesOutput.cursor.ns, listIndexesOutput);

    let indexKeys = listIndexesOutput.cursor.firstBatch.map(x => x.key);
    assert.sameMembers(
        [{[subField1]: 1}, {[subField2]: 1}, {[timeField]: 1}, {[metaField]: 1}].concat(
            extraIndexes),
        indexKeys);

    assert.commandWorked(coll.dropIndex({[subField2]: 1}));
    indexKeys = coll.getIndexKeys();
    assert.sameMembers([{[subField1]: 1}, {[timeField]: 1}, {[metaField]: 1}].concat(extraIndexes),
                       indexKeys);

    plan = coll.find({[metaField]: 0}).explain();
    assert.eq(getAggPlanStages(plan, "IXSCAN").length, 2, plan);

    assert.commandWorked(coll.dropIndex({[metaField]: 1}));
    indexKeys = coll.getIndexKeys();
    assert.sameMembers([{[subField1]: 1}, {[timeField]: 1}].concat(extraIndexes), indexKeys);

    plan = coll.find({[subField2]: 0}).explain();
    assert.eq(getAggPlanStages(plan, "COLLSCAN").length, 2, plan);

    // Verify that running the commands directly on the raw buckets works.
    const outputOnRawBuckets =
        assert.commandWorked(getTimeseriesCollForRawOps(mongosDB, coll).runCommand({
            listIndexes: getTimeseriesCollForRawOps(mongosDB, coll).getName(),
            ...getRawOperationSpec(mongosDB)
        }));
    assert.eq(getTimeseriesCollForDDLOps(mongosDB, coll).getFullName(),
              outputOnRawBuckets.cursor.ns,
              outputOnRawBuckets);

    assert.commandWorked(getTimeseriesCollForRawOps(mongosDB, coll)
                             .dropIndexes({'meta.subField1': 1}, getRawOperationSpec(mongosDB)));

    indexKeys =
        getTimeseriesCollForRawOps(mongosDB, coll).getIndexKeys(getRawOperationSpec(mongosDB));
    const indexesToAdd = {'control.min.time': 1, "control.max.time": 1};
    assert.sameMembers([indexesToAdd].concat(extraBucketIndexes), indexKeys);

    assert.commandWorked(createRawTimeseriesIndex(coll, {'meta.subField2': 1}));
    indexKeys =
        getTimeseriesCollForRawOps(mongosDB, coll).getIndexKeys(getRawOperationSpec(mongosDB));
    assert.sameMembers([indexesToAdd, {'meta.subField2': 1}].concat(extraBucketIndexes), indexKeys);

    assert(coll.drop());
})();

st.stop();
