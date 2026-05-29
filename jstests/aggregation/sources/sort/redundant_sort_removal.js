/**
 * Tests that the REDUNDANT_SORT_REMOVAL rule removes a $sort whose output order is already
 * guaranteed by the preceding stage.
 *
 * @tags: [
 *   requires_pipeline_optimization,
 *   do_not_wrap_aggregations_in_facets,
 *   requires_fcv_90
 * ]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStages, getPlanStages} from "jstests/libs/query/analyze_plan.js";

describe("REDUNDANT_SORT_REMOVAL rule", function () {
    const coll = db.redundant_sort_removal;

    before(function () {
        coll.drop();
        for (let a = 0; a < 3; a++) {
            for (let b = 0; b < 3; b++) {
                assert.commandWorked(coll.insertOne({a, b}));
            }
        }
    });

    // Returns true if actualKey has exactly the same fields and directions as expectedKey.
    function keyMatches(actualKey, expectedKey) {
        if (!actualKey) return false;
        for (const [field, dir] of Object.entries(expectedKey)) {
            if (actualKey[field] !== dir) return false;
        }
        for (const field of Object.keys(actualKey)) {
            if (!(field in expectedKey)) return false;
        }
        return true;
    }

    const sortMatchesKey = (s, key) => keyMatches(s.$sort && s.$sort.sortKey, key);
    const qpSortMatchesKey = (s, key) => keyMatches(s.sortPattern, key);

    // Asserts actual and reference have the same ordered values for the given fields.
    function assertOrderedFieldsMatch(actual, reference, fields, label) {
        assert.eq(actual.length, reference.length, label + ": length mismatch");
        for (let i = 0; i < actual.length; i++) {
            for (const f of fields) {
                assert.eq(actual[i][f], reference[i][f], `${label}: row ${i} '${f}' mismatch`);
            }
        }
    }

    // Counts $sort stages matching key in the explained plan and calls assertFn(count, expected).
    // Prefers aggregation $sort stages; falls back to query-planner SORT stages only when the agg
    // count is zero. The fallback handles topologies (e.g. auth standalone) where $sort is pushed
    // entirely into the query planner. getAggPlanStages does not cover splitPipeline, so sharded
    // explains need splitPipeline.shardsPart and mergerPart checked separately to avoid falling
    // through to the per-shard QP count (which would inflate the total by the number of shards).
    function assertSortCount(pipeline, key, assertFn, expected, label) {
        const explained = coll.explain().aggregate(pipeline);
        const splitStages = [
            ...(explained.splitPipeline?.shardsPart ?? []),
            ...(explained.splitPipeline?.mergerPart ?? []),
        ];
        const aggCount =
            getAggPlanStages(explained, "$sort").filter((s) => sortMatchesKey(s, key)).length +
            splitStages.filter((s) => sortMatchesKey(s, key)).length;
        const qpCount = getPlanStages(explained, "SORT").filter((s) => qpSortMatchesKey(s, key)).length;
        const count = aggCount > 0 ? aggCount : qpCount;
        assertFn(count, expected, label + ": " + tojson(explained));
    }

    // [$sort{a,b}, $limit{5}, $sort{a}]: $limit{5} is absorbed into $sort{a,b}, leaving the two
    // sorts adjacent. REDUNDANT_SORT_REMOVAL then erases $sort{a}.
    it("superset+limit [$sort{a,b}, $limit{5}, $sort{a}] erases second sort", function () {
        const pipeline = [{$sort: {a: 1, b: 1}}, {$limit: 5}, {$sort: {a: 1}}];
        assertOrderedFieldsMatch(
            coll.aggregate(pipeline).toArray(),
            coll.aggregate([{$sort: {a: 1, b: 1}}, {$limit: 5}]).toArray(),
            ["a", "b"],
            "superset+limit",
        );
        assertSortCount(pipeline, {a: 1}, assert.eq, 0, "expected $sort{a} to be removed");
    });

    // [$sort{a,b}, $match{a>0}, $limit{5}, $sort{a}]: after MATCH_PUSHDOWN moves $match before
    // $sort{a,b}, the limit is absorbed and REDUNDANT_SORT_REMOVAL erases $sort{a}.
    it("match pushdown makes sorts adjacent: erases trailing $sort{a}", function () {
        const pipeline = [{$sort: {a: 1, b: 1}}, {$match: {a: {$gt: 0}}}, {$limit: 5}, {$sort: {a: 1}}];
        assertOrderedFieldsMatch(
            coll.aggregate(pipeline).toArray(),
            coll.aggregate([{$match: {a: {$gt: 0}}}, {$sort: {a: 1, b: 1}}, {$limit: 5}]).toArray(),
            ["a", "b"],
            "match pushdown",
        );
        assertSortCount(pipeline, {a: 1}, assert.eq, 0, "expected $sort{a} removed after match pushdown");
    });

    // [$sort{a,b}, $limit{5}, $group{_id:"$a"}, $sort{a}]: $group cannot be pushed past $sort, so
    // it stays between the two sorts. REDUNDANT_SORT_REMOVAL sees $group (empty sort pattern) as
    // the preceding stage and does not erase $sort{a}.
    it("non-pushable $group between sorts blocks removal: keeps $sort{a}", function () {
        const pipeline = [{$sort: {a: 1, b: 1}}, {$limit: 5}, {$group: {_id: "$a"}}, {$sort: {a: 1}}];
        const withSort = coll.aggregate(pipeline).toArray();
        const reference = coll.aggregate([{$sort: {a: 1, b: 1}}, {$limit: 5}, {$group: {_id: "$a"}}]).toArray();
        assert.eq(withSort.length, reference.length, "length mismatch");
        assertSortCount(pipeline, {a: 1}, assert.gte, 1, "expected $sort{a} to remain (not erased)");
    });

    // [$sort{a}, $limit{5}, $sort{a,b}]: $sort{a} does NOT guarantee {a,b} order, so
    // REDUNDANT_SORT_REMOVAL must NOT erase $sort{a,b}.
    it("insufficient prefix [$sort{a}, $limit{5}, $sort{a,b}] keeps $sort{a,b}", function () {
        const pipeline = [{$sort: {a: 1}}, {$limit: 5}, {$sort: {a: 1, b: 1}}];
        const withSort = coll.aggregate(pipeline).toArray();
        const reference = coll.aggregate([{$sort: {a: 1, b: 1}}, {$limit: 5}]).toArray();
        assert.eq(withSort.length, reference.length, "length mismatch");
        assertSortCount(pipeline, {a: 1, b: 1}, assert.gte, 1, "expected $sort{a,b} to remain (insufficient prefix)");
    });

    // [$match{a>0}, $sort{a}]: backward scan reaches $match which has no sort pattern so the rule
    // does not fire.
    it("no preceding sort pattern [$match{a>0}, $sort{a}] keeps $sort{a}", function () {
        const pipeline = [{$match: {a: {$gt: 0}}}, {$sort: {a: 1}}];
        assertSortCount(pipeline, {a: 1}, assert.gte, 1, "expected $sort{a} to remain (no preceding sort pattern)");
    });
});
