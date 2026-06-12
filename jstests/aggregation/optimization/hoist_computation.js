/**
 * Tests the behavior of $project and $addFields computation hoisting.
 *
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   requires_pipeline_optimization,
 *   # Uses runWithParamsAllNonConfigNodes which requires a stable shard list.
 *   assumes_stable_shard_list,
 *   # Uses a knob (internalQueryTransformHoistPolicy) that does not exist on older binaries.
 *   multiversion_incompatible,
 *   assumes_unsharded_collection,
 *   # $group serialization changes depending on FCV (adds $willBeMerged) which breaks the test.
 *   cannot_run_during_upgrade_downgrade,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {assertArrayEq, inhibitOptimizationPerStage} from "jstests/aggregation/extras/utils.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {it} from "jstests/libs/mochalite.js";

if (!FeatureFlagUtil.isPresentAndEnabled(db, "featureFlagImprovedDepsAnalysis")) {
    jsTest.log.info("Skipping test - featureFlagImprovedDepsAnalysis is not present");
    quit();
}

// Collection with single document to use in $lookup.
db.hoist_computation_secondary.drop();
assert.commandWorked(db.hoist_computation_secondary.insert({}));

/// Extracts the user-visible aggregation stages.
function extractUserStages(explain) {
    return (explain.stages || []).filter(
        (stage) => !stage.$cursor && !stage.$_internalInhibitOptimization,
    );
}

/// Returns true if `actual` matches `expected`, allowing any leading
/// $set/$addFields/$project/$match stages in `expected` to be absent because SBE pushdown
/// can absorb them into the $cursor stage.
function stagesMatch(actual, expected) {
    const sbeAbsorbableStages = ["$set", "$addFields", "$project", "$match"];
    const maybePushedDownToSbe = (stage) => sbeAbsorbableStages.some((s) => s in stage);
    for (let i = 0; i <= expected.length; i++) {
        if (i > 0 && !maybePushedDownToSbe(expected[i - 1])) {
            break;
        }
        if (friendlyEqual(actual, expected.slice(i))) {
            return true;
        }
    }
    return false;
}

/// Produces a lookup stage which stores [{fromLookup: 1}] in the 'as' field and has some optional field references.
function lookupStage(as, references = []) {
    return {
        $lookup: {
            from: "hoist_computation_secondary",
            pipeline: [{$count: "fromLookup"}],
            let: Object.fromEntries(references.map((ref, i) => ["var" + i, ref])),
            as: as,
        },
    };
}

/**
 * Run a test
 * @param name of the test case
 * @param docs to initialise the pipeline
 * @param pipeline input pipeline
 * @param optimized the expected optimized pipeline
 * @param expected the expected results
 * @param policy the required TransformHoistPolicy for the rewrite
 */
function runTest({name, docs, pipeline, optimized, expected, policy}) {
    assert(typeof policy === "string");
    function runTestInner(expectedOptimized) {
        const coll = db.hoist_computation;
        coll.drop();
        assert.commandWorked(coll.insertMany(docs));

        const stripId = ({_id, ...rest}) => rest;

        // Verify the unoptimized pipeline produces the expected results (golden baseline).
        const unoptimizedResults = coll
            .aggregate(inhibitOptimizationPerStage(pipeline))
            .toArray()
            .map(stripId);
        assert.docEq(unoptimizedResults, expected, "unoptimized results");

        // Verify the original pipeline's explain matches the expected optimized pipeline.
        // Check plans before results so a plan mismatch clarifies any subsequent result failure.
        const explain = coll.explain().aggregate(pipeline);
        const actualStages = extractUserStages(explain);

        // Insert $_internalInhibitOptimization before each stage in the expected optimized
        // pipeline to prevent the optimizer from further modifying it during explain.
        const inhibitedOptimized = inhibitOptimizationPerStage(expectedOptimized);
        const optimizedExplain = coll.explain().aggregate(inhibitedOptimized);
        const expectedStages = extractUserStages(optimizedExplain);

        assert(stagesMatch(actualStages, expectedStages), "not optimized as expected", {
            actualStages,
            expectedStages,
        });

        // Verify the original pipeline produces the same results after optimization.
        const results = coll.aggregate(pipeline).toArray().map(stripId);
        assert.docEq(results, expected, "optimized results");
    }

    function runWithPolicy(p, expectedOptimized) {
        runWithParamsAllNonConfigNodes(db, {internalQueryTransformHoistPolicy: p}, () =>
            runTestInner(expectedOptimized),
        );
    }

    switch (policy) {
        case "always":
            it(name + " (always)", () => {
                runWithPolicy("always", optimized);
            });
            it(name + " (forMatchPushdown)", () => {
                runWithPolicy("forMatchPushdown", pipeline);
            });
            break;
        case "forMatchPushdown":
            it(name, () => {
                runWithPolicy("forMatchPushdown", optimized);
            });
            break;
        default:
            assert(false);
    }
}

runTest({
    name: "independent: constant $set on unrelated field is pushed down",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a"), {$set: {b: 1}}],
    optimized: [{$set: {b: 1}}, lookupStage("a")],
    expected: [
        {a: [{fromLookup: 1}], b: 1},
        {b: 1, a: [{fromLookup: 1}]},
    ],
    policy: "always",
});

runTest({
    name: "rename: $set with rename is pushed down",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a"), {$set: {b: "$c"}}],
    optimized: [{$set: {b: "$c"}}, lookupStage("a")],
    expected: [{a: [{fromLookup: 1}]}, {a: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "rename: $set with complex rename is pushed down",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a"), {$set: {b: "$c.d"}}],
    optimized: [{$set: {b: "$c.d"}}, lookupStage("a")],
    expected: [{a: [{fromLookup: 1}]}, {a: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "rename: $set with deep rename is pushed down",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a"), {$set: {b: "$c.d.e"}}],
    optimized: [{$set: {b: "$c.d.e"}}, lookupStage("a")],
    expected: [{a: [{fromLookup: 1}]}, {a: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "rename: $set with dotted new name is push down",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a"), {$set: {"b.c": "$d"}}],
    optimized: [{$set: {"b.c": "$d"}}, lookupStage("a")],
    expected: [
        {a: [{fromLookup: 1}], b: {}},
        {b: {}, a: [{fromLookup: 1}]},
    ],
    policy: "always",
});

runTest({
    name: "true dependency: $set expression reads lookup output field prevents hoist",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a"), {$set: {b: {$isArray: "$a"}}}],
    optimized: [lookupStage("a"), {$set: {b: {$isArray: "$a"}}}],
    expected: [
        {a: [{fromLookup: 1}], b: true},
        {b: true, a: [{fromLookup: 1}]},
    ],
    policy: "always",
});

runTest({
    name: "true dependency: $set expression reads parent of lookup output path prevents hoist",
    docs: [{c: 1}],
    pipeline: [lookupStage("a.b"), {$set: {result: {$gt: ["$a", null]}}}],
    optimized: [lookupStage("a.b"), {$set: {result: {$gt: ["$a", null]}}}],
    expected: [{c: 1, a: {b: [{fromLookup: 1}]}, result: true}],
    policy: "always",
});

runTest({
    name: "true dependency: $set expression reads sibling of lookup output path is pushed down",
    docs: [{a: {c: 5}, d: 1}],
    pipeline: [lookupStage("a.b"), {$set: {result: {$gt: ["$a.c", null]}}}],
    optimized: [{$set: {result: {$gt: ["$a.c", null]}}}, lookupStage("a.b")],
    expected: [{a: {c: 5, b: [{fromLookup: 1}]}, d: 1, result: true}],
    policy: "always",
});

runTest({
    name: "true dependency: $set expression reads child of lookup output path prevents hoist",
    docs: [{c: 1}],
    pipeline: [lookupStage("a"), {$set: {result: {$isArray: "$a.fromLookup"}}}],
    optimized: [lookupStage("a"), {$set: {result: {$isArray: "$a.fromLookup"}}}],
    expected: [{c: 1, a: [{fromLookup: 1}], result: true}],
    policy: "always",
});

runTest({
    name: "true dependency: $set expression reads unrelated dotted path is pushed down",
    docs: [{b: {fromLookup: 5}, d: 1}],
    pipeline: [lookupStage("a"), {$set: {result: {$gt: ["$b.fromLookup", null]}}}],
    optimized: [{$set: {result: {$gt: ["$b.fromLookup", null]}}}, lookupStage("a")],
    expected: [{b: {fromLookup: 5}, d: 1, a: [{fromLookup: 1}], result: true}],
    policy: "always",
});

runTest({
    name: "whole document dependency: $set using $$ROOT is not hoisted",
    docs: [{c: 1}],
    pipeline: [lookupStage("a"), {$set: {b: {$type: "$$ROOT"}}}],
    optimized: [lookupStage("a"), {$set: {b: {$type: "$$ROOT"}}}],
    expected: [{c: 1, a: [{fromLookup: 1}], b: "object"}],
    policy: "always",
});

runTest({
    name: "whole document dependency: $set using $$CURRENT is not hoisted",
    docs: [{c: 1}],
    pipeline: [lookupStage("a"), {$set: {b: {$type: "$$CURRENT"}}}],
    optimized: [lookupStage("a"), {$set: {b: {$type: "$$CURRENT"}}}],
    expected: [{c: 1, a: [{fromLookup: 1}], b: "object"}],
    policy: "always",
});

runTest({
    name: "anti-dependency: lookup let reads $set output field prevents hoist",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a", ["$b"]), {$set: {b: 1}}],
    optimized: [lookupStage("a", ["$b"]), {$set: {b: 1}}],
    expected: [
        {a: [{fromLookup: 1}], b: 1},
        {b: 1, a: [{fromLookup: 1}]},
    ],
    policy: "always",
});

runTest({
    name: "anti-dependency: lookup reads child path via let, $set writes parent prevents hoist",
    docs: [{a: {b: 0}, c: 1}],
    pipeline: [lookupStage("x", ["$a.b"]), {$set: {a: 99}}],
    optimized: [lookupStage("x", ["$a.b"]), {$set: {a: 99}}],
    expected: [{a: 99, c: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "anti-dependency: lookup reads child path via let, $set writes sibling is pushed down",
    docs: [{a: {b: 5, c: 0}, d: 1}],
    pipeline: [lookupStage("x", ["$a.b"]), {$set: {"a.c": 99}}],
    optimized: [{$set: {"a.c": 99}}, lookupStage("x", ["$a.b"])],
    expected: [{a: {b: 5, c: 99}, d: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "anti-dependency: lookup reads parent path via let, $set writes child prevents hoist",
    docs: [{a: {b: 0}, c: 1}],
    pipeline: [lookupStage("x", ["$a"]), {$set: {"a.b": 99}}],
    optimized: [lookupStage("x", ["$a"]), {$set: {"a.b": 99}}],
    expected: [{a: {b: 99}, c: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "anti-dependency: lookup reads sibling path via let, $set writes other sibling is pushed down",
    docs: [{a: {b: 0, c: 5}, d: 1}],
    pipeline: [lookupStage("x", ["$a.c"]), {$set: {"a.b": 99}}],
    optimized: [{$set: {"a.b": 99}}, lookupStage("x", ["$a.c"])],
    expected: [{a: {b: 99, c: 5}, d: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "anti-dependency: lookup reads exact dotted path via let, $set writes same dotted path prevents hoist",
    docs: [{a: {b: 5}, c: 1}],
    pipeline: [lookupStage("x", ["$a.b"]), {$set: {"a.b": 99}}],
    optimized: [lookupStage("x", ["$a.b"]), {$set: {"a.b": 99}}],
    expected: [{a: {b: 99}, c: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "output dependency: $set writes same field as lookup output prevents hoist",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a"), {$set: {a: 1}}],
    optimized: [lookupStage("a"), {$set: {a: 1}}],
    expected: [
        {a: 1, b: 0},
        {b: 0, a: 1},
    ],
    policy: "always",
});

runTest({
    name: "output dependency: $set writes parent of lookup output path prevents hoist",
    docs: [{c: 1}],
    pipeline: [lookupStage("a.b"), {$set: {a: 99}}],
    optimized: [lookupStage("a.b"), {$set: {a: 99}}],
    expected: [{c: 1, a: 99}],
    policy: "always",
});

runTest({
    name: "output dependency: $set writes sibling of lookup output path is pushed down",
    docs: [{d: 1}],
    pipeline: [lookupStage("a.b"), {$set: {"a.c": 99}}],
    optimized: [{$set: {"a.c": 99}}, lookupStage("a.b")],
    expected: [{d: 1, a: {b: [{fromLookup: 1}], c: 99}}],
    policy: "always",
});

runTest({
    name: "output dependency: $set writes child of lookup output path prevents hoist",
    docs: [{c: 1}],
    pipeline: [lookupStage("a"), {$set: {"a.b": 99}}],
    optimized: [lookupStage("a"), {$set: {"a.b": 99}}],
    expected: [{c: 1, a: [{fromLookup: 1, b: 99}]}],
    policy: "always",
});

runTest({
    name: "output dependency: $set writes sibling of lookup child output path is pushed down",
    docs: [{d: 1}],
    pipeline: [lookupStage("a.c"), {$set: {"a.b": 99}}],
    optimized: [{$set: {"a.b": 99}}, lookupStage("a.c")],
    expected: [{d: 1, a: {c: [{fromLookup: 1}], b: 99}}],
    policy: "always",
});

runTest({
    name: "$set split+hoist: intra-$set data dependency prevents partial hoist",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a"), {$set: {a: "$b", b: 1}}],
    optimized: [lookupStage("a"), {$set: {a: "$b", b: 1}}],
    expected: [
        {a: 0, b: 1},
        {b: 1, a: 0},
    ],
    policy: "always",
});

runTest({
    name: "$set split+hoist: independent field splits before lookup, conflicting field stays",
    docs: [
        {a: 0, b: 0},
        {b: 0, a: 0},
    ],
    pipeline: [lookupStage("a"), {$set: {a: 1, b: 1}}],
    optimized: [{$set: {b: 1}}, lookupStage("a"), {$set: {a: 1}}],
    expected: [
        {a: 1, b: 1},
        {b: 1, a: 1},
    ],
    policy: "always",
});

runTest({
    name: "$set hoist when dotted paths are independent",
    docs: [{x: {y: 0}, c: 1}],
    pipeline: [lookupStage("a.b"), {$set: {"x.y": 99}}],
    optimized: [{$set: {"x.y": 99}}, lookupStage("a.b")],
    expected: [{x: {y: 99}, c: 1, a: {b: [{fromLookup: 1}]}}],
    policy: "always",
});

runTest({
    name: "$set split+hoist: independent dotted path splits, conflicting child path stays",
    docs: [{x: {y: 0}, c: 1}],
    pipeline: [lookupStage("a"), {$set: {"a.b": 1, "x.y": 1}}],
    optimized: [{$set: {"x.y": 1}}, lookupStage("a"), {$set: {"a.b": 1}}],
    expected: [{x: {y: 1}, c: 1, a: [{fromLookup: 1, b: 1}]}],
    policy: "always",
});

runTest({
    name: "$set split+hoist: residual reads hoistable path prevents split via cascade",
    docs: [{a: 5, b: 0, c: 1}],
    pipeline: [lookupStage("out"), {$set: {out: "$a", a: 42}}],
    optimized: [lookupStage("out"), {$set: {out: "$a", a: 42}}],
    expected: [{a: 42, b: 0, c: 1, out: 5}],
    policy: "always",
});

runTest({
    name: "$set split+hoist: three-level cascade pins all fields",
    docs: [{a: 5, b: 3, c: 1}],
    pipeline: [lookupStage("out"), {$set: {out: "$a", a: "$b", b: 42}}],
    optimized: [lookupStage("out"), {$set: {out: "$a", a: "$b", b: 42}}],
    expected: [{a: 3, b: 42, c: 1, out: 5}],
    policy: "always",
});

runTest({
    name: "$set split+hoist: residual reads parent of hoistable dotted path prevents split via cascade",
    docs: [{b: {c: 0}, d: 1}],
    pipeline: [lookupStage("out"), {$set: {out: "$b", "b.c": 42}}],
    optimized: [lookupStage("out"), {$set: {out: "$b", "b.c": 42}}],
    expected: [{b: {c: 42}, d: 1, out: {c: 0}}],
    policy: "always",
});

runTest({
    name: "$set split+hoist: residual reads sibling of hoistable dotted path allows split",
    docs: [{b: {c: 5, d: 0}, e: 1}],
    pipeline: [lookupStage("out"), {$set: {out: "$b.c", "b.d": 42}}],
    optimized: [{$set: {"b.d": 42}}, lookupStage("out"), {$set: {out: "$b.c"}}],
    expected: [{b: {c: 5, d: 42}, e: 1, out: 5}],
    policy: "always",
});

/// ------------------------------------
/// Exclusion projection
/// ------------------------------------

runTest({
    name: "$unset: $unset of unrelated field is not pushed down",
    docs: [{a: 1, b: 1}],
    pipeline: [lookupStage("x"), {$unset: "b"}],
    optimized: [lookupStage("x"), {$unset: "b"}],
    expected: [{a: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "$unset: $unset of lookup output field is not pushed down",
    docs: [{a: 1, b: 1}],
    pipeline: [lookupStage("x"), {$unset: "x"}],
    optimized: [lookupStage("x"), {$unset: "x"}],
    expected: [{a: 1, b: 1}],
    policy: "always",
});

/// ------------------------------------
/// Multiple hoistable stages
/// ------------------------------------

runTest({
    name: "two $set stages: both hoisted before lookup, relative order preserved",
    docs: [{a: 0, c: 1}],
    pipeline: [lookupStage("x"), {$set: {a: 1}}, {$set: {b: "$a"}}],
    optimized: [{$set: {a: 1}}, {$set: {b: "$a"}}, lookupStage("x")],
    expected: [{a: 1, b: 1, c: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "three $set stages: all hoisted before lookup, relative order preserved",
    docs: [{a: 0, b: 0, d: 1}],
    pipeline: [lookupStage("x"), {$set: {a: 1}}, {$set: {b: "$a"}}, {$set: {c: "$b"}}],
    optimized: [{$set: {a: 1}}, {$set: {b: "$a"}}, {$set: {c: "$b"}}, lookupStage("x")],
    expected: [{a: 1, b: 1, c: 1, d: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "three independent $set stages: all hoisted before lookup in unspecified order",
    docs: [{e: 1}],
    pipeline: [lookupStage("x"), {$set: {a: 42}}, {$set: {b: 7}}, {$set: {c: 3}}],
    optimized: [{$set: {a: 42}}, {$set: {b: 7}}, {$set: {c: 3}}, lookupStage("x")],
    expected: [{a: 42, b: 7, c: 3, e: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "two $set stages with computed expressions: both hoisted before lookup, relative order preserved",
    docs: [{a: 0, c: 1}],
    pipeline: [lookupStage("x"), {$set: {a: {$add: ["$a", 1]}}}, {$set: {b: {$add: ["$a", 1]}}}],
    optimized: [{$set: {a: {$add: ["$a", 1]}}}, {$set: {b: {$add: ["$a", 1]}}}, lookupStage("x")],
    expected: [{a: 1, b: 2, c: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "three $set stages with computed expressions: all hoisted before lookup, relative order preserved",
    docs: [{a: 0, b: 0, d: 1}],
    pipeline: [
        lookupStage("x"),
        {$set: {a: {$add: ["$a", 1]}}},
        {$set: {b: {$add: ["$a", 1]}}},
        {$set: {c: {$add: ["$b", 1]}}},
    ],
    optimized: [
        {$set: {a: {$add: ["$a", 1]}}},
        {$set: {b: {$add: ["$a", 1]}}},
        {$set: {c: {$add: ["$b", 1]}}},
        lookupStage("x"),
    ],
    expected: [{a: 1, b: 2, c: 3, d: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

runTest({
    name: "three independent $set stages with computed expressions: all hoisted before lookup in unspecified order",
    docs: [{a: 0, b: 0, c: 0, e: 1}],
    pipeline: [
        lookupStage("x"),
        {$set: {a: {$add: ["$a", 1]}}},
        {$set: {b: {$add: ["$b", 1]}}},
        {$set: {c: {$add: ["$c", 1]}}},
    ],
    optimized: [
        {$set: {a: {$add: ["$a", 1]}}},
        {$set: {b: {$add: ["$b", 1]}}},
        {$set: {c: {$add: ["$c", 1]}}},
        lookupStage("x"),
    ],
    expected: [{a: 1, b: 1, c: 1, e: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

/// ------------------------------------
/// barrier cases (previous stage type or constraint prevents hoisting)
/// ------------------------------------

runTest({
    name: "match barrier: $set not hoisted before $match to prevent rewrite loop",
    docs: [{a: 1, c: 1}],
    pipeline: [lookupStage("x"), {$match: {x: {$exists: true}}}, {$set: {b: 1}}],
    optimized: [lookupStage("x"), {$match: {x: {$exists: true}}}, {$set: {b: 1}}],
    expected: [{a: 1, b: 1, c: 1, x: [{fromLookup: 1}]}],
    policy: "always",
});

it("position requirement barrier: $set not hoisted before stage with position requirement", () => {
    // This is a useful test at the time of writing, since $indexStats (DocumentSourceQueue)
    // surprisingly reports empty modified paths, despite being a new source for the pipeline,
    // thereby allowing this rewrite, unless we make sure to check the position requirement.
    // Position requirement is already validated internally, we just need to make sure we don't crash.
    runWithParamsAllNonConfigNodes(db, {internalQueryTransformHoistPolicy: "always"}, () => {
        db.hoist_computation.aggregate([{$indexStats: {}}, {$set: {a: 1}}]).toArray();
    });
});

it("maximum paths knob: blocks hoisting above the limit", () => {
    runWithParamsAllNonConfigNodes(
        db,
        {
            internalQueryTransformHoistPolicy: "always",
            internalQueryTransformHoistMaximumPaths: 2,
        },
        () => {
            const coll = db.hoist_computation;
            coll.drop();
            assert.commandWorked(coll.insertMany([{a: 0, b: 0, c: 0}]));

            const assertPlan = (input, expected) => {
                const actualStages = extractUserStages(coll.explain().aggregate(input));
                const expectedStages = extractUserStages(
                    coll.explain().aggregate(inhibitOptimizationPerStage(expected)),
                );
                assert(stagesMatch(actualStages, expectedStages), "not optimized as expected", {
                    actualStages,
                    expectedStages,
                });
            };

            assertPlan(
                [lookupStage("x"), {$set: {a: 1, b: 2, c: 3}}],
                [lookupStage("x"), {$set: {a: 1, b: 2, c: 3}}],
            );
            assertPlan(
                [lookupStage("x"), {$set: {a: 1, b: 2}}],
                [{$set: {a: 1, b: 2}}, lookupStage("x")],
            );
        },
    );
});

/// ------------------------------------
/// sanity check cases
/// ------------------------------------

runTest({
    name: "$group: $set not hoisted",
    docs: [{}],
    pipeline: [lookupStage("a"), {$group: {_id: "$a"}}, {$set: {a: "$_id", b: 1}}],
    optimized: [lookupStage("a"), {$group: {_id: "$a"}}, {$set: {a: "$_id", b: 1}}],
    expected: [{a: [{fromLookup: 1}], b: 1}],
    policy: "always",
});

/// ------------------------------------
/// $project+$match pushdown cases follow
/// ------------------------------------

// This test verifies that we apply match pushdown first before we attempt to hoist.
runTest({
    name: "$project+$match pushdown: $match pushdown before hoisting",
    docs: [{a: 5, c: 1}],
    pipeline: [lookupStage("x"), {$project: {c: "$a", x: 1}}, {$match: {c: 1}}],
    optimized: [lookupStage("x"), {$project: {c: "$a", x: 1}}],
    expected: [],
    policy: "forMatchPushdown",
});

runTest({
    name: "$project+$match pushdown: independent $set hoisted before $lookup when $match follows",
    docs: [{a: 5, c: 1}],
    pipeline: [lookupStage("x"), {$set: {sum: {$add: ["$a", 1]}}}, {$match: {sum: {$gt: 3}}}],
    optimized: [{$set: {sum: {$add: ["$a", 1]}}}, {$match: {sum: {$gt: 3}}}, lookupStage("x")],
    expected: [{a: 5, c: 1, sum: 6, x: [{fromLookup: 1}]}],
    policy: "forMatchPushdown",
});

runTest({
    name: "$project+$match pushdown: before $lookup",
    docs: [{a: 5, c: 1}],
    pipeline: [
        lookupStage("x"),
        {$project: {sum: {$add: ["$a", 1]}, a: 1, c: 1}},
        {$match: {sum: {$gt: 3}}},
    ],
    optimized: [
        {$addFields: {sum: {$add: ["$a", 1]}}},
        {$match: {sum: {$gt: 3}}},
        lookupStage("x"),
        {$project: {sum: 1, a: 1, c: 1}},
    ],
    expected: [{a: 5, c: 1, sum: 6}],
    policy: "forMatchPushdown",
});

runTest({
    name: "computed field and match pushed before chained lookup+unwind pairs",
    docs: [
        {a: 0, b: 0, x: 5},
        {b: 0, a: 0, x: 5},
    ],
    pipeline: [
        lookupStage("b"),
        lookupStage("c"),
        {$project: {sum: {$add: ["$a", "$x"]}, a: 1, b: 1, c: 1}},
        {$match: {sum: {$lt: 100}}},
    ],
    optimized: [
        {$addFields: {sum: {$add: ["$a", "$x"]}}},
        {$match: {sum: {$lt: 100}}},
        lookupStage("b"),
        lookupStage("c"),
        {$project: {sum: 1, a: 1, b: 1, c: 1}},
    ],
    expected: [
        {a: 0, b: [{fromLookup: 1}], c: [{fromLookup: 1}], sum: 5},
        {b: [{fromLookup: 1}], a: 0, c: [{fromLookup: 1}], sum: 5},
    ],
    policy: "forMatchPushdown",
});

runTest({
    name: "inclusion $project with _id exclusion",
    docs: [{a: 0}],
    pipeline: [lookupStage("x"), {$project: {_id: 0, b: "$a"}}],
    optimized: [{$addFields: {b: "$a"}}, lookupStage("x"), {$project: {_id: 0, b: 1}}],
    expected: [{b: 0}],
    policy: "always",
});

runTest({
    name: "inclusion $project with _id exclusion and computed",
    docs: [{a: 0}],
    pipeline: [lookupStage("x"), {$project: {_id: 0, b: "$a", x: 1}}],
    optimized: [{$addFields: {b: "$a"}}, lookupStage("x"), {$project: {_id: 0, b: 1, x: 1}}],
    expected: [{b: 0, x: [{fromLookup: 1}]}],
    policy: "always",
});

/// ------------------------------------
/// $match pushdown inside a $lookup inner pipeline ($sequentialCache interaction)
/// ------------------------------------

it("match pushdown in $lookup inner pipeline does not crash in $sequentialCache serving mode (SERVER-127822)", () => {
    // The correlated $lookup inner pipeline below has an uncorrelated leading $match, a chain of
    // single-document $project transformations, and a trailing correlated $match. The match-pushdown
    // rule (PUSH_MATCH_BEFORE_SINGLE_DOC_TRANSFORMATION) pushes the correlated $match ahead of the
    // $project stages. On the 2nd outer document the $sequentialCache is in "serving" mode and erases
    // the uncorrelated prefix; postTransform() then tries to resize the dependency graph from the
    // (now-erased) last stage. The test makes sure this doesn't cause a tassert (error 12299003).
    // Two outer documents are required so the cache reaches serving mode.
    runWithParamsAllNonConfigNodes(
        db,
        {internalQueryTransformHoistPolicy: "forMatchPushdown"},
        () => {
            const coll = db.hoist_computation;
            coll.drop();
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, bar: 53},
                    {_id: 1, bar: 100},
                ]),
            );

            const pipeline = [
                {
                    $lookup: {
                        from: coll.getName(),
                        let: {outerId: "$_id"},
                        pipeline: [
                            // Uncorrelated leading $match: cached as the uncorrelated prefix and erased
                            // in serving mode once the correlated $match is pushed in front of the
                            // $project stages.
                            {$match: {$expr: {$gt: ["$_id", {$literal: null}]}}},
                            // A chain of single-document transformations for the correlated $match to be
                            // pushed in front of.
                            {$project: {wrapped: "$$ROOT", _id: 0}},
                            {$project: {inner: {id: "$wrapped._id", bar: "$wrapped.bar"}, _id: 0}},
                            {$project: {u: "$inner", _id: 0}},
                            // Correlated $match referencing the let variable.
                            {$match: {$expr: {$eq: ["$$outerId", "$u.id"]}}},
                        ],
                        as: "joined",
                    },
                },
            ];

            // Each outer document self-joins with the inner document having the same _id.
            assertArrayEq({
                actual: coll.aggregate(pipeline).toArray(),
                expected: [
                    {_id: 0, bar: 53, joined: [{u: {id: 0, bar: 53}}]},
                    {_id: 1, bar: 100, joined: [{u: {id: 1, bar: 100}}]},
                ],
            });
        },
    );
});
