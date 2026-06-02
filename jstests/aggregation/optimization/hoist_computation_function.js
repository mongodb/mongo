/**
 * Tests that $set stages containing $function expressions are not hoisted before $lookup, because
 * $function may have side-effects or depend on execution order.
 *
 * @tags: [
 *   requires_scripting,
 *   requires_pipeline_optimization,
 *   # Uses a knob (internalQueryTransformHoistPolicy) that does not exist on older binaries.
 *   multiversion_incompatible,
 *   assumes_unsharded_collection,
 *   assumes_stable_shard_list,
 *   # Wrapping in $facet changes the explain structure so the stage-order check below cannot
 *   # inspect the original pipeline stages.
 *   do_not_wrap_aggregations_in_facets,
 *   # featureFlagImprovedDepsAnalysis behavior can differ across FCV boundaries.
 *   cannot_run_during_upgrade_downgrade,
 *   # TODO SERVER-116052: Add support for $function.
 *   mozjs_wasm_unsupported,
 * ]
 */
import {it} from "jstests/libs/mochalite.js";

db.hoist_computation_function_secondary.drop();
assert.commandWorked(db.hoist_computation_function_secondary.insert({}));

it("$function side effects: outer $set with $function not hoisted before $lookup", function () {
    const coll = db.hoist_computation_function;
    coll.drop();
    coll.insertOne({c: 1});

    const readDistinctTime = function () {
        var now = Date.now();
        while (Date.now() === now) {}
        return new Date(now);
    };

    const timeFunction = {$function: {body: readDistinctTime, args: [], lang: "js"}};

    // The first $set is nested inside the $lookup sub-pipeline and captures time1.
    // It spins until the clock ticks so time1 is at a strictly earlier millisecond.
    // The outer $set captures time2 after the lookup completes.
    // If the optimizer hoisted the outer $set before the $lookup, time2 < time1, which is wrong.
    const pipeline = [
        {
            $lookup: {
                from: "hoist_computation_function_secondary",
                pipeline: [{$set: {time1: timeFunction}}],
                as: "result",
            },
        },
        {$set: {time2: timeFunction}},
    ];

    // Verify the outer $set is not hoisted before the $lookup.
    // In a mongos passthrough the stages live under explain.shards[...].stages, not at the top
    // level, so explain.stages is absent; skip the stage-order check and rely on the timing
    // check below for correctness.
    const explain = coll.explain().aggregate(pipeline);
    if (explain.stages) {
        const stages = explain.stages.map((s) => Object.keys(s)[0]);
        assert.eq(
            stages.filter((s) => s === "$lookup" || s === "$set"),
            ["$lookup", "$set"],
            {stages},
        );
    }

    // Verify execution order: time1 (set inside the lookup) must precede time2 (set after).
    const [
        {
            result: [{time1}],
            time2,
        },
    ] = coll.aggregate(pipeline).toArray();
    assert.lt(time1.getTime(), time2.getTime(), tojson({time1, time2}));
});
