/**
 * Tests explaining write operations on a time-series buckets collection.
 *
 * @tags: [
 *   requires_fcv_82,
 *   requires_timeseries,
 *   known_query_shape_computation_problem,  # TODO (SERVER-103069): Remove this tag.
 * ]
 */

import {
    getTimeseriesCollForRawOps,
    kRawOperationSpec
} from "jstests/core/libs/raw_operation_utils.js";
import {getPlanStage} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];

const timeField = "t";
const metaField = "m";
const time = new Date("2024-01-01T00:00:00Z");

coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

assert.commandWorked(coll.insert([
    {[timeField]: time, [metaField]: 1, a: "a"},
    {[timeField]: time, [metaField]: 2, a: "b"},
    {[timeField]: time, [metaField]: 2, a: "c"},
]));

const assertExplain = function(commandResult, commandName) {
    assert(commandResult.ok);
    if (commandResult.command.bulkWrite) {
        assert.eq(commandResult.command.nsInfo.length,
                  1,
                  `Expected 1 namespace in explain command but got ${
                      commandResult.command.nsInfo.length}`);
        assert.eq(commandResult.command.nsInfo[0].ns,
                  coll.getFullName(),
                  `Expected command namespace to be ${tojson(coll.getFullName())} but got ${
                      tojson(commandResult.command.nsInfo[0].ns)}`);
    } else {
        assert.eq(commandResult.command[commandName],
                  coll.getName(),
                  `Expected command namespace to be ${tojson(coll.getName())} but got ${
                      tojson(commandResult.command[commandName])}`);
    }
    assert(commandResult.command.rawData);
    assert.isnull(getPlanStage(commandResult, "TS_MODIFY")),
        "Expected not to find TS_MODIFY stage " + tojson(commandResult);
};

assertExplain(getTimeseriesCollForRawOps(coll).explain().findAndModify({
    query: {"control.count": 2},
    update: {$set: {meta: "3"}},
    ...kRawOperationSpec,
}),
              "findAndModify");
assertExplain(
    getTimeseriesCollForRawOps(coll).explain().remove({"control.count": 2}, kRawOperationSpec),
    "delete");
assertExplain(getTimeseriesCollForRawOps(coll).explain().update(
                  {"control.count": 1}, {$set: {meta: "3"}}, kRawOperationSpec),
              "update");

// Additionally run explains that issue a cluster write without a shard key in a sharded environment
// to test that path.
assertExplain(getTimeseriesCollForRawOps(coll).explain().remove(
                  {"control.count": 2}, {...kRawOperationSpec, justOne: true}),
              "delete");
assertExplain(getTimeseriesCollForRawOps(coll).explain().update(
                  {"_id": 1}, {$set: {meta: "3"}}, kRawOperationSpec),
              "update");
