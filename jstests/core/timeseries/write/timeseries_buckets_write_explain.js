/**
 * Tests explaining write operations on a time-series buckets collection.
 *
 * @tags: [
 *   featureFlagRawDataCrudOperations,
 *   requires_timeseries,
 * ]
 */

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
    assert.eq(commandResult.command[commandName],
              coll.getName(),
              `Expected command namespace to be ${tojson(coll.getName())} but got ${
                  tojson(commandResult.command[commandName])}`);

    assert(commandResult.command.rawData);
    assert.isnull(getPlanStage(commandResult, "TS_MODIFY")),
        "Expected not to find TS_MODIFY stage " + tojson(commandResult);
};

assertExplain(coll.explain().findAndModify({
    query: {"control.count": 2},
    update: {$set: {meta: "3"}},
    rawData: true,
}),
              "findAndModify");
assertExplain(coll.explain().remove({"control.count": 2}, {rawData: true}), "delete");
assertExplain(coll.explain().update({"control.count": 1}, {$set: {meta: "3"}}, {rawData: true}),
              "update");

// Additionally run an explain that issues a cluster write without a shard key to test that
// path.
// TODO SERVER-102697: Cluster write without shard key for findAndModify (if not put into its own
// test).
assertExplain(coll.explain().remove({"control.count": 2}, {rawData: true, justOne: true}),
              "delete");
assertExplain(coll.explain().update({"_id": 1}, {$set: {meta: "3"}}, {rawData: true}), "update");
