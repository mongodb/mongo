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
const bucketsColl = db["system.buckets." + coll.getName()];

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

// TODO SERVER-100926, SERVER-100929: This function will no longer need the rawData argument after
// these tickets.
const assertExplain = function(commandResult, commandName, rawData) {
    assert(commandResult.ok);
    assert.eq(commandResult.command[commandName],
              rawData ? coll.getName() : bucketsColl.getName(),
              `Expected command namespace to be ${tojson(coll.getName())} but got ${
                  tojson(commandResult.command[commandName])}`);

    if (rawData) {
        assert.eq(commandResult.command.rawData, rawData);
        assert.isnull(getPlanStage(commandResult, "TS_MODIFY")),
            "Expected not to find TS_MODIFY stage " + tojson(commandResult);
    }
};

assertExplain(
    bucketsColl.explain().findAndModify({query: {"control.count": 2}, update: {$set: {meta: "3"}}}),
    "findAndModify",
    false);
assertExplain(bucketsColl.explain().remove({"control.count": 2}), "delete", false);
// Explain update on a field that uses cluster write without shard key and one that doesn't.
assertExplain(coll.explain().update({"control.count": 1}, {$set: {meta: "3"}}, {rawData: true}),
              "update",
              true);
assertExplain(
    coll.explain().update({"_id": 1}, {$set: {meta: "3"}}, {rawData: true}), "update", true);
