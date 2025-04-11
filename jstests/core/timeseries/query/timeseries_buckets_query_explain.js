/**
 * Tests explaining read operations on a time-series buckets collection.
 *
 * @tags: [
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   featureFlagRawDataCrudOperations,
 *   known_query_shape_computation_problem,  # TODO (SERVER-103069): Remove this tag.
 *   requires_timeseries,
 * ]
 */
import {
    getTimeseriesCollForRawOps,
    getTimeseriesExecutionNamespace
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

const assertQueryPlannerNamespace = function(explain) {
    if (explain.shards) {
        for (const shardExplain of Object.values(explain.shards)) {
            assert.eq(
                shardExplain.queryPlanner.namespace,
                getTimeseriesExecutionNamespace(getTimeseriesCollForRawOps(coll)).getFullName(),
                `Expected shard plan query planner namespace to be ${
                    tojson(getTimeseriesExecutionNamespace(getTimeseriesCollForRawOps(coll))
                               .getFullName())} but got ${tojson(shardExplain)}`);
        }
    } else if (explain.queryPlanner.namespace) {
        assert.eq(explain.queryPlanner.namespace,
                  getTimeseriesExecutionNamespace(getTimeseriesCollForRawOps(coll)).getFullName(),
                  `Expected query planner namespace to be ${
                      tojson(getTimeseriesExecutionNamespace(getTimeseriesCollForRawOps(coll))
                                 .getFullName())} but got ${tojson(explain)}`);
    } else {
        for (const shardPlan of explain.queryPlanner.winningPlan.shards) {
            assert.eq(
                shardPlan.namespace,
                getTimeseriesExecutionNamespace(getTimeseriesCollForRawOps(coll)).getFullName(),
                `Expected winning shard plan query planner namespace to be ${
                    tojson(getTimeseriesExecutionNamespace(
                        getTimeseriesCollForRawOps(coll)))} but got ${tojson(shardPlan)}`);
        }
    }
};

const assertCommandNamespace = function(explain, commandRun) {
    assert.eq(
        explain.command[commandRun],
        coll.getName(),
        `Expected command namespace to be ${tojson(coll.getName())} but got ${tojson(explain)}`);
};

const assertExplain = function(explain, commandRun) {
    assertQueryPlannerNamespace(explain);
    assertCommandNamespace(explain, commandRun);
    assert(explain.command.rawData);
    assert(!getPlanStage(explain, "UNPACK_TS_BUCKET"),
           `Expected to find no unpack stage but got ${tojson(explain)}`);
};

assertExplain(coll.explain().aggregate([{$match: {"control.count": 2}}], {rawData: true}),
              "aggregate");
assertExplain(coll.explain().count({"control.count": 2}, {rawData: true}), "count");
assertExplain(coll.explain().distinct("control.count", {}, {rawData: true}), "distinct");
assertExplain(coll.explain().find({"control.count": 2}).rawData().finish(), "find");
