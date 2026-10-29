/**
 * Verify SBE native $minN/$maxN group accumulator usage and correctness.
 *
 * @tags: [
 *   uses_explain,
 *   requires_sbe,
 *   cannot_run_during_upgrade_downgrade,
 *   # Explain does not support stepdowns.
 *   does_not_support_stepdowns,
 *   # Explain cannot run within a multi-document transaction.
 *   does_not_support_transactions,
 *   requires_fcv_91,
 *   # The implicit timeseries collection conversion changes the $group plan shape and results.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */
import {getEngine, getNestedProperties, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    coll.insert([
        {_id: 1, groupKey: "a", v: 1},
        {_id: 2, groupKey: "a", v: 2},
    ]),
);

function sbeStages(pipeline, c = coll) {
    const explain = c.explain().aggregate(pipeline);
    assert.eq(getEngine(explain), "sbe", explain);
    return getQueryPlanners(explain).flatMap((qp) =>
        getNestedProperties(qp, "slotBasedPlan").flatMap((plan) => plan.stages ?? []),
    );
}

assert(
    sbeStages([{$group: {_id: "$groupKey", m: {$minN: {n: 10, input: "$v"}}}}]).every((s) =>
        s.includes("_internalMinN"),
    ),
);
assert(
    sbeStages([{$group: {_id: "$groupKey", m: {$maxN: {n: 10, input: "$v"}}}}]).every((s) =>
        s.includes("_internalMaxN"),
    ),
);

const idPipeline = [{$group: {_id: "$groupKey", minVals: {$minN: {n: 3, input: "$_id"}}}}];
assert(sbeStages(idPipeline).every((s) => s.includes("_internalMinN")));
assert.eq(coll.aggregate(idPipeline).toArray(), [{_id: "a", minVals: [1, 2]}]);

const dynColl = db[jsTestName() + "_dynamic"];
dynColl.drop();
assert.commandWorked(
    dynColl.insert([
        {_id: 1, gk: "a", v: 1},
        {_id: 2, gk: "b", v: 10},
        {_id: 3, gk: "a", v: 2},
        {_id: 4, gk: "b", v: 20},
        {_id: 5, gk: "a", v: 3},
        {_id: 6, gk: "b", v: 30},
    ]),
);

const dynamicN = {$cond: {if: {$eq: ["$gk", "a"]}, then: 3, else: 1}};
const dynamicPipeline = [{$group: {_id: {gk: "$gk"}, m: {$minN: {n: dynamicN, input: "$v"}}}}];
assert(sbeStages(dynamicPipeline, dynColl).every((s) => s.includes("_internalMinN")));
assert.eq(
    dynColl
        .aggregate(dynamicPipeline)
        .toArray()
        .sort((x, y) => (x._id.gk < y._id.gk ? -1 : 1)),
    [
        {_id: {gk: "a"}, m: [1, 2, 3]},
        {_id: {gk: "b"}, m: [10]},
    ],
);
