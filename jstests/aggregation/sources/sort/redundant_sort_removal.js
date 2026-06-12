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
    const planStageSortMatchesKey = (s, key) => keyMatches(s.sortPattern, key);

    // Asserts actual and reference have the same ordered values for the given fields.
    function assertOrderedFieldsMatch(actual, reference, fields, label) {
        assert.eq(actual.length, reference.length, label + ": length mismatch");
        for (let i = 0; i < actual.length; i++) {
            for (const f of fields) {
                assert.eq(actual[i][f], reference[i][f], `${label}: row ${i} '${f}' mismatch`);
            }
        }
    }

    // Counts $sort stages matching key across ALL plan layers (aggregation pipeline stages and query
    // planner 'PlanStage' nodes) and calls
    // assertFn(count, expected). Summing both layers lets exact-match tests distinguish "trailing
    // sort removed" from "trailing sort kept." Tests that check for 0 use a prefix-subset key that
    // doesn't match the superset query-planner sort pattern, so planStageCount stays 0 for those.
    // getAggPlanStages does not cover splitPipeline, so sharded explains check shardsPart and
    // mergerPart separately. In sharded context, the $sort in shardsPart and the per-shard physical
    // SORT plan nodes represent the same operation, so only aggCount is used when splitPipeline
    // exists to avoid double-counting.
    function assertSortCount(pipeline, key, assertFn, expected, label) {
        const explained = coll.explain().aggregate(pipeline);
        const splitStages = [
            ...(explained.splitPipeline?.shardsPart ?? []),
            ...(explained.splitPipeline?.mergerPart ?? []),
        ];
        const aggCount =
            getAggPlanStages(explained, "$sort").filter((s) => sortMatchesKey(s, key)).length +
            splitStages.filter((s) => sortMatchesKey(s, key)).length;
        const planStageCount = getPlanStages(explained, "SORT").filter((s) => planStageSortMatchesKey(s, key)).length;
        const count = explained.splitPipeline ? aggCount : aggCount + planStageCount;
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

    // ---- $replaceRoot / $replaceWith as an intervening stage before the redundant $sort ----
    //
    // A "transparent" $replaceRoot/$replaceWith is one whose newRoot expression maps every sort key
    // field to itself: {a: "$a", b: "$b"}. This is analogous to an identity inclusion projection
    // where document order is preserved AND the sort key field values are unchanged, enabling the
    // backward scan to safely proceed. A $replaceRoot that renames or computes a sort key
    // field (e.g., {a: "$b"} or {a: {$add: ["$a", 1]}}) is NOT transparent for that sort pattern;
    // the upstream ordering guarantee no longer applies to the output field names and the trailing
    // sort must stay.
    //
    // TODO SERVER-127594: extend to $project, $addFields/$set/$unset once audited.

    it("$replaceRoot intervening: exact multi-field sort match removed", function () {
        const pipeline = [{$sort: {a: 1, b: 1}}, {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}}, {$sort: {a: 1, b: 1}}];
        assertOrderedFieldsMatch(
            coll.aggregate(pipeline).toArray(),
            coll.aggregate([{$sort: {a: 1, b: 1}}, {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}}]).toArray(),
            ["a", "b"],
            "$replaceRoot exact match",
        );
        assertSortCount(
            pipeline,
            {a: 1, b: 1},
            assert.eq,
            1,
            "expected trailing $sort{a,b} removed through $replaceRoot",
        );
    });

    it("$replaceWith intervening: descending superset sort removed", function () {
        const pipeline = [{$sort: {a: -1, b: -1}}, {$replaceWith: {a: "$a", b: "$b"}}, {$sort: {a: -1}}];
        assertOrderedFieldsMatch(
            coll.aggregate(pipeline).toArray(),
            coll.aggregate([{$sort: {a: -1, b: -1}}, {$replaceWith: {a: "$a", b: "$b"}}]).toArray(),
            ["a", "b"],
            "$replaceWith descending",
        );
        assertSortCount(pipeline, {a: -1}, assert.eq, 0, "expected $sort{a:-1} removed after $replaceWith");
    });

    // [$sort{a:-1,b:-1}, $replaceWith{a:"$a",b:"$b"}, $sort{a:1}]: initial sort is descending but
    // trailing sort is ascending so the final $sort must remain.
    it("$replaceWith intervening: opposite direction keeps trailing sort", function () {
        const pipeline = [{$sort: {a: -1, b: -1}}, {$replaceWith: {a: "$a", b: "$b"}}, {$sort: {a: 1}}];
        assertSortCount(
            pipeline,
            {a: 1},
            assert.gte,
            1,
            "expected $sort{a:1} kept: direction mismatch with preceding $sort{a:-1}",
        );
    });

    it("$replaceRoot renames sort key field: sort on new name kept", function () {
        const pipeline = [{$sort: {a: 1}}, {$replaceRoot: {newRoot: {b: "$a"}}}, {$sort: {b: 1}}];
        assertSortCount(pipeline, {b: 1}, assert.gte, 1, "expected $sort{b} kept: preceding sort is on 'a', not 'b'");
    });

    // [$sort{a,b}, $replaceRoot{a:"$a",b:"$b"}, $sort{a}]: $replaceRoot identity-maps every field
    // referenced by the trailing $sort{a:1}, so the backward scan continues past it, finds the
    // covering $sort{a:1,b:1}, and erases $sort{a:1}.
    it("$replaceRoot identity-maps all sort key fields: superset covering sort removes trailing sort", function () {
        const pipeline = [{$sort: {a: 1, b: 1}}, {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}}, {$sort: {a: 1}}];
        assertOrderedFieldsMatch(
            coll.aggregate(pipeline).toArray(),
            coll.aggregate([{$sort: {a: 1, b: 1}}, {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}}]).toArray(),
            ["a", "b"],
            "$replaceRoot identity-maps sort key fields",
        );
        assertSortCount(
            pipeline,
            {a: 1},
            assert.eq,
            0,
            "expected $sort{a} removed through identity-mapping $replaceRoot",
        );
    });

    // [$sort{a,b}, $replaceRoot{a:"$b",b:"$b"}, $sort{a}]: newRoot maps "$b" onto "a", overwriting
    // the sort key field with a different value, so the trailing sort stays.
    it("$replaceRoot overwrites sort key field with another field: trailing sort kept", function () {
        const pipeline = [{$sort: {a: 1, b: 1}}, {$replaceRoot: {newRoot: {a: "$b", b: "$b"}}}, {$sort: {a: 1}}];
        assertSortCount(
            pipeline,
            {a: 1},
            assert.gte,
            1,
            "expected $sort{a} kept: $replaceRoot overwrites 'a' with '$b'",
        );
    });

    it("two consecutive transparent stages ($replaceRoot then $replaceWith): sort removed", function () {
        const pipeline = [
            {$sort: {a: 1, b: 1}},
            {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}},
            {$replaceWith: {a: "$a", b: "$b"}},
            {$sort: {a: 1}},
        ];
        assertOrderedFieldsMatch(
            coll.aggregate(pipeline).toArray(),
            coll
                .aggregate([
                    {$sort: {a: 1, b: 1}},
                    {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}},
                    {$replaceWith: {a: "$a", b: "$b"}},
                ])
                .toArray(),
            ["a", "b"],
            "two transparent stages",
        );
        assertSortCount(
            pipeline,
            {a: 1},
            assert.eq,
            0,
            "expected $sort{a} removed: scan continues past both transparent stages",
        );
    });

    // [$sort{a,b}, $replaceRoot, $sort{a}, $sort{a}]: two consecutive trailing sorts separated from
    // the leading sort by a transparent stage. REDUNDANT_SORT_REMOVAL removes the last $sort{a}
    // (redundant given the middle $sort{a}), then removes the middle $sort{a} (redundant given
    // $sort{a,b} through $replaceRoot). Without the $replaceRoot separator, SORT_MERGE
    // would collapse all three sorts into one $sort{a} before REDUNDANT_SORT_REMOVAL runs.
    it("multiple consecutive redundant sorts through transparent stage: both trailing $sort{a} removed", function () {
        const pipeline = [
            {$sort: {a: 1, b: 1}},
            {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}},
            {$sort: {a: 1}},
            {$sort: {a: 1}},
        ];
        assertOrderedFieldsMatch(
            coll.aggregate(pipeline).toArray(),
            coll.aggregate([{$sort: {a: 1, b: 1}}, {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}}]).toArray(),
            ["a", "b"],
            "consecutive redundant sorts through transparent",
        );
        assertSortCount(pipeline, {a: 1}, assert.eq, 0, "expected both trailing $sort{a} removed");
    });

    // [$sort{a,b}, $replaceRoot, $sort{a}, $replaceWith, $sort{a}]: two non-consecutive trailing
    // sorts, each separated from the preceding sort by one transparent stage.
    // REDUNDANT_SORT_REMOVAL walks through each transparent stage to find the preceding sort and
    // removes both trailing sorts.
    it("non-consecutive redundant sorts separated by transparent stages: both removed", function () {
        const pipeline = [
            {$sort: {a: 1, b: 1}},
            {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}},
            {$sort: {a: 1}},
            {$replaceWith: {a: "$a", b: "$b"}},
            {$sort: {a: 1}},
        ];
        assertOrderedFieldsMatch(
            coll.aggregate(pipeline).toArray(),
            coll
                .aggregate([
                    {$sort: {a: 1, b: 1}},
                    {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}},
                    {$replaceWith: {a: "$a", b: "$b"}},
                ])
                .toArray(),
            ["a", "b"],
            "non-consecutive transparent",
        );
        assertSortCount(
            pipeline,
            {a: 1},
            assert.eq,
            0,
            "expected both trailing $sort{a} removed through transparent stages",
        );
    });

    // [$sort{a,b}, $replaceRoot, $sort{a}, $group{_id:"$a"}, $sort{a}]: the middle $sort{a} is
    // removed (backward scan walks through transparent $replaceRoot to find $sort{a,b}), but the
    // final $sort{a} is kept (backward scan hits $group which has no sort pattern).
    it("blocking stage keeps later $sort while transparent stage removes earlier $sort", function () {
        const pipeline = [
            {$sort: {a: 1, b: 1}},
            {$replaceRoot: {newRoot: {a: "$a", b: "$b"}}},
            {$sort: {a: 1}},
            {$group: {_id: "$a"}},
            {$sort: {a: 1}},
        ];
        assertSortCount(
            pipeline,
            {a: 1},
            assert.eq,
            1,
            "expected last $sort{a} kept (blocked by $group), middle $sort{a} removed",
        );
    });

    // [$sort{a,b}, $replaceRoot{a:"$b",b:"$a"}, $sort{a,b}]: $replaceRoot swaps sort key field
    // values. Even though this stage technically sets preservesOrderAndMetadata=true, the upstream
    // sort guarantee for 'a' and 'b' no longer holds after the swap, so the trailing $sort{a,b}
    // must NOT be removed.
    it("$replaceRoot that swaps sort key fields: trailing sort kept", function () {
        const pipeline = [{$sort: {a: 1, b: 1}}, {$replaceRoot: {newRoot: {a: "$b", b: "$a"}}}, {$sort: {a: 1, b: 1}}];
        assertSortCount(
            pipeline,
            {a: 1, b: 1},
            assert.gte,
            2,
            "expected trailing $sort{a,b} kept: $replaceRoot swaps sort key field values",
        );
    });
});
