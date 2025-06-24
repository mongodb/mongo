/**
 * Tests explaining a findAndModify with rawData that doesn't specify a shard key.
 *
 * @tags: [
 *  requires_replication,
 *  requires_sharding,
 *  requires_timeseries,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getPlanStage} from "jstests/libs/query/analyze_plan.js";
import {
    getRawOperationSpec,
    getTimeseriesCollForRawOps,
    isRawOperationSupported
} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const assertExplain = function(coll, commandResult) {
    const commandName = "findAndModify";
    assert(commandResult.ok);
    assert.eq(commandResult.command[commandName],
              coll.getName(),
              `Expected command namespace to be ${tojson(coll.getName())} but got ${
                  tojson(commandResult.command[commandName])}`);
    assert(isRawOperationSupported(db) === (commandResult.command.rawData ?? false));
    assert.isnull(getPlanStage(commandResult, "TS_MODIFY")),
        "Expected not to find TS_MODIFY stage " + tojson(commandResult);
};

const metaFieldName = "meta";
const timeFieldName = "time";
const dbName = "test";

const st = new ShardingTest({shards: 2});
const db = st.s0.getDB(dbName);

const coll = db[jsTestName()];
assert.commandWorked(db.adminCommand({
    shardCollection: coll.getFullName(),
    key: {[metaFieldName]: 1},
    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
}));

coll.insertMany([
    {[timeFieldName]: ISODate(), [metaFieldName]: 1, data: 1},
    {[timeFieldName]: ISODate(), [metaFieldName]: 2, data: 2},
    {[timeFieldName]: ISODate(), [metaFieldName]: 3, data: 3},
    {[timeFieldName]: ISODate(), [metaFieldName]: 4, data: 4},
]);

assert.commandWorked(db.adminCommand(
    {split: getTimeseriesCollForDDLOps(db, coll).getFullName(), middle: {[metaFieldName]: 2}}));

const primaryShard = st.getPrimaryShard(dbName);
const otherShard = st.getOther(primaryShard);
assert.commandWorked(db.adminCommand({
    moveChunk: getTimeseriesCollForDDLOps(db, coll).getFullName(),
    find: {[metaFieldName]: 1},
    to: otherShard.shardName,
}));

assertExplain(getTimeseriesCollForRawOps(db, coll),
              getTimeseriesCollForRawOps(db, coll).explain().findAndModify({
                  query: {"control.count": 1},
                  update: {$set: {meta: "3"}},
                  ...getRawOperationSpec(db),
              }));

st.stop();
