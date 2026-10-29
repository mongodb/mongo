/**
 * Tests extension-to-extension optimization rule combinations:
 *   - eraseVectorSearchAt1: fires when pos 1 is $testVectorSearch; tested via a stage that expands
 * to place it there.
 *   - eraseVectorSearchAt2: fires when pos 2 is $testVectorSearch (one desugared stage sits between
 * them).
 *
 * @tags: [
 *   featureFlagExtensionsAPI
 * ]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const coll = db.getCollection(jsTestName());

////////////////////////////////////////////////////////////////////////////////////////////////////
// Rule: eraseVectorSearchAt1 (non-desugarable stage registers rule involving desugarable and
// non-desugarable stages)
//
// Fires when pos 1 is $testVectorSearch. The second pipeline stage expands into
// [$testVectorSearch, ...], placing $testVectorSearch at pos 1. Tests check explain stage count
// since the stage is a passthrough and result length can't distinguish one run vs two.
////////////////////////////////////////////////////////////////////////////////////////////////////

describe("eraseVectorSearchAt1", function () {
    let shardMultiplier;

    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, x: 1},
                {_id: 2, x: 2},
            ]),
        );
        // In a sharded cluster, getAggPlanStages counts stages across all shards.
        // Derive the multiplier from a single-stage pipeline where no rule fires.
        const baselineExplain = coll.explain("queryPlanner").aggregate([{$testVectorSearch: {}}]);
        shardMultiplier = getAggPlanStages(baselineExplain, "$testVectorSearch").length;
    });

    it("erases B's $testVectorSearch when B is storedSource:true", function () {
        const pipeline = [
            {$testVectorSearch: {}},
            {$testVectorSearchOptimization: {storedSource: true}},
        ];
        const explain = coll.explain("queryPlanner").aggregate(pipeline);
        const count = getAggPlanStages(explain, "$testVectorSearch").length;
        assert.eq(
            count,
            1 * shardMultiplier,
            "Expected eraseVectorSearchAt1 to fire when B desugars to storedSource:true",
            {explain},
        );
    });

    it("erases B's $testVectorSearch when B is storedSource:false", function () {
        const pipeline = [
            {$testVectorSearch: {}},
            {$testVectorSearchOptimization: {storedSource: false}},
        ];
        const explain = coll.explain("queryPlanner").aggregate(pipeline);
        const count = getAggPlanStages(explain, "$testVectorSearch").length;
        assert.eq(
            count,
            1 * shardMultiplier,
            "Expected eraseVectorSearchAt1 to fire when B desugars to storedSource:false",
            {explain},
        );
    });

    it("does NOT fire when A is storedSource:true and B is a direct $testVectorSearch", function () {
        // $replaceRoot sits at pos 1, not $testVectorSearch, so rule does not fire.
        const pipeline = [
            {$testVectorSearchOptimization: {storedSource: true}},
            {$addFields: {a: 1}},
            {$testVectorSearch: {}},
        ];
        const explain = coll.explain("queryPlanner").aggregate(pipeline);
        const count = getAggPlanStages(explain, "$testVectorSearch").length;
        assert.eq(
            count,
            2 * shardMultiplier,
            "Expected eraseVectorSearchAt1 to NOT fire when $replaceRoot intervenes",
            {explain},
        );
    });
});

////////////////////////////////////////////////////////////////////////////////////////////////////
// Rule: eraseVectorSearchAt2 (desugarable stage registers rule involving desugarable stage)
//
// Fires when pos 2 is $testVectorSearch. The preceding $testVectorSearch expands with one
// intermediate stage ($replaceRoot or $_internalSearchIdLookup), placing the following
// $testVectorSearch at pos 2. Also checks explain stage count.
////////////////////////////////////////////////////////////////////////////////////////////////////

describe("eraseVectorSearchAt2", function () {
    let shardMultiplier;

    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, x: 1},
                {_id: 2, x: 2},
            ]),
        );
        // In a sharded cluster, getAggPlanStages counts stages across all shards.
        // Derive the multiplier from a single-stage pipeline where no rule fires.
        const baselineExplain = coll.explain("queryPlanner").aggregate([{$testVectorSearch: {}}]);
        shardMultiplier = getAggPlanStages(baselineExplain, "$testVectorSearch").length;
    });

    it("erases B's $testVectorSearch: storedSource:true followed by storedSource:true", function () {
        const pipeline = [
            {$testVectorSearchOptimization: {storedSource: true}},
            {$testVectorSearchOptimization: {storedSource: true}},
        ];
        const explain = coll.explain("queryPlanner").aggregate(pipeline);
        const count = getAggPlanStages(explain, "$testVectorSearch").length;
        assert.eq(
            count,
            1 * shardMultiplier,
            "Expected eraseVectorSearchAt2 to fire for storedSource:true + storedSource:true",
            {
                explain,
            },
        );
    });

    it("erases B's $testVectorSearch: storedSource:true followed by storedSource:false", function () {
        const pipeline = [
            {$testVectorSearchOptimization: {storedSource: true}},
            {$testVectorSearchOptimization: {storedSource: false}},
        ];
        const explain = coll.explain("queryPlanner").aggregate(pipeline);
        const count = getAggPlanStages(explain, "$testVectorSearch").length;
        assert.eq(
            count,
            1 * shardMultiplier,
            "Expected eraseVectorSearchAt2 to fire for storedSource:true + storedSource:false",
            {
                explain,
            },
        );
    });

    it("erases B's $testVectorSearch: storedSource:false followed by storedSource:true", function () {
        const pipeline = [
            {$testVectorSearchOptimization: {storedSource: false}},
            {$testVectorSearchOptimization: {storedSource: true}},
        ];
        const explain = coll.explain("queryPlanner").aggregate(pipeline);
        const count = getAggPlanStages(explain, "$testVectorSearch").length;
        assert.eq(
            count,
            1 * shardMultiplier,
            "Expected eraseVectorSearchAt2 to fire for storedSource:false + storedSource:true",
            {
                explain,
            },
        );
    });

    it("erases B's $testVectorSearch: storedSource:false followed by storedSource:false", function () {
        const pipeline = [
            {$testVectorSearchOptimization: {storedSource: false}},
            {$testVectorSearchOptimization: {storedSource: false}},
        ];
        const explain = coll.explain("queryPlanner").aggregate(pipeline);
        const count = getAggPlanStages(explain, "$testVectorSearch").length;
        assert.eq(
            count,
            1 * shardMultiplier,
            "Expected eraseVectorSearchAt2 to fire for storedSource:false + storedSource:false",
            {
                explain,
            },
        );
    });

    it("erases B's $testVectorSearch: storedSource:true followed by direct $testVectorSearch", function () {
        const pipeline = [
            {$testVectorSearchOptimization: {storedSource: true}},
            {$testVectorSearch: {}},
        ];
        const explain = coll.explain("queryPlanner").aggregate(pipeline);
        const count = getAggPlanStages(explain, "$testVectorSearch").length;
        assert.eq(
            count,
            1 * shardMultiplier,
            "Expected eraseVectorSearchAt2 to fire for storedSource:true + direct $testVectorSearch",
            {
                explain,
            },
        );
    });

    it("does NOT fire when an $addFields follows $replaceRoot, shifting B to position 3", function () {
        // $addFields follows $replaceRoot, so pos 2 is $addFields, not $testVectorSearch.
        const pipeline = [
            {$testVectorSearchOptimization: {storedSource: true}},
            {$addFields: {a: 1}},
            {$testVectorSearchOptimization: {storedSource: true}},
        ];
        const explain = coll.explain("queryPlanner").aggregate(pipeline);
        const count = getAggPlanStages(explain, "$testVectorSearch").length;
        assert.eq(
            count,
            2 * shardMultiplier,
            "Expected eraseVectorSearchAt2 to NOT fire when $addFields shifts B to position 3",
            {
                explain,
            },
        );
    });
});
