/**
 * Tests that a $unwind on a top-level field followed by a $group on the same field with no
 * accumulators is answered by a DISTINCT_SCAN over a (possibly multikey) index. This is only
 * possible with preserveNullAndEmptyArrays, because without it documents with nullish values
 * produce no group at all while the index cannot distinguish them from e.g. documents with a
 * [null] element.
 *
 * @tags: [
 *   # The sharding and $facet passthrough suites modify aggregation pipelines in a way that
 *   # prevents the DISTINCT_SCAN optimization from being applied, which breaks the test.
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   # Explain assertions depend on which indexes exist.
 *   assumes_no_implicit_index_creation,
 *   requires_fcv_90,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];

const unwindA = {$unwind: {path: "$a", preserveNullAndEmptyArrays: true}};
const groupOnA = {$group: {_id: "$a"}};

function setupCollection(docs, indexes = [{a: 1}]) {
    coll.drop();
    assert.commandWorked(coll.insertMany(docs));
    for (const index of indexes) {
        assert.commandWorked(coll.createIndex(index.key ?? index, index.options));
    }
}

function assertResultsAndPlan({
    pipeline,
    expectedResults,
    expectDistinctScan,
    options = {},
    expectedControlResults = expectedResults,
    expectUnwindsArrays = true,
}) {
    const controlResults = coll.aggregate(pipeline, {...options, hint: {$natural: 1}}).toArray();
    assertArrayEq({actual: controlResults, expected: expectedControlResults});

    const results = coll.aggregate(pipeline, options).toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    const explain = coll.explain().aggregate(pipeline, options);
    const distinctScans = getAggPlanStages(explain, "DISTINCT_SCAN");
    if (expectDistinctScan) {
        assert.gt(distinctScans.length, 0, explain);
        for (const stage of distinctScans) {
            assert.eq(!!stage.unwindsArrays, expectUnwindsArrays, explain);
        }
    } else {
        assert.eq(distinctScans.length, 0, explain);
    }
}

describe("$unwind+$group to DISTINCT_SCAN optimization", function () {
    describe("basic cases", function () {
        it("answers the example from SERVER-33715 with a multikey index", function () {
            for (const index of [{a: 1}, {a: -1}]) {
                setupCollection([{a: [1, 2, 3]}, {a: [2, 3, 4]}], [index]);
                assertResultsAndPlan({
                    pipeline: [unwindA, groupOnA],
                    expectedResults: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
                    expectDistinctScan: true,
                });
            }
        });

        it("handles a mix of arrays and scalars", function () {
            setupCollection([{a: [1, 2]}, {a: 2}, {a: 7}, {a: [1]}, {a: [8, 9]}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 7}, {_id: 8}, {_id: 9}],
                expectDistinctScan: true,
            });
        });

        it("does not unwind nested arrays recursively", function () {
            setupCollection([{a: [[1, 2], 3]}, {a: [[1, 2], 4]}, {a: [[]]}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: [1, 2]}, {_id: 3}, {_id: 4}, {_id: []}],
                expectDistinctScan: true,
            });
        });

        it("works with a non-multikey index", function () {
            setupCollection([{a: 1}, {a: 2}, {b: 1}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: null}],
                expectDistinctScan: true,
            });
        });

        it("does not set unwindsArrays in explain for a $group without $unwind", function () {
            setupCollection([{a: 1}, {a: 2}, {b: 1}]);
            assertResultsAndPlan({
                pipeline: [groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: null}],
                expectDistinctScan: true,
                expectUnwindsArrays: false,
            });
        });

        it("works with a compound index where the unwound field comes first", function () {
            setupCollection(
                [
                    {a: [1, 2], b: 1},
                    {a: 3, b: 2},
                ],
                [{a: 1, b: 1}],
            );
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 3}],
                expectDistinctScan: true,
            });
        });

        it("works with compound indexes of mixed directions", function () {
            // Without a sort we are free to scan a descending index backwards, which is required
            // when _replaceUndefinedWithNull=true in the distinct scan stage.
            for (const index of [
                {a: -1, b: 1},
                {a: 1, b: -1},
                {a: -1, b: -1},
            ]) {
                setupCollection(
                    [
                        {a: [1, 2], b: 1},
                        {a: 3, b: 2},
                    ],
                    [index],
                );
                assertResultsAndPlan({
                    pipeline: [unwindA, groupOnA],
                    expectedResults: [{_id: 1}, {_id: 2}, {_id: 3}],
                    expectDistinctScan: true,
                });
            }
        });
    });

    describe("nullish values", function () {
        it("collapses missing, null, empty array and [null] into a single null group", function () {
            for (const index of [{a: 1}, {a: -1}]) {
                setupCollection([{b: 1}, {a: null}, {a: []}, {a: [null, 1]}, {a: 2}], [index]);
                assertResultsAndPlan({
                    pipeline: [unwindA, groupOnA],
                    expectedResults: [{_id: null}, {_id: 1}, {_id: 2}],
                    expectDistinctScan: true,
                });
            }
        });

        it("produces the null group for an empty array alone", function () {
            setupCollection([{a: []}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: null}],
                expectDistinctScan: true,
            });
        });

        it("handles nullish values nested inside array elements correctly", function () {
            for (const index of [{a: 1}, {a: -1}]) {
                setupCollection(
                    [
                        {a: [null]},
                        {a: [null, null]},
                        {a: [[null]]},
                        {a: [[undefined]]},
                        {a: [[[null]]]},
                        {a: [[[undefined]]]},
                        {a: [[[]]]},
                        {a: [[null], [undefined]]},
                        {a: [[null], []]},
                        {a: [[undefined], []]},
                        {a: [[], []]},
                    ],
                    [index],
                );
                assertResultsAndPlan({
                    pipeline: [unwindA, groupOnA],
                    expectedResults: [
                        {_id: null},
                        {_id: []},
                        {_id: [null]},
                        {_id: [undefined]},
                        {_id: [[null]]},
                        {_id: [[undefined]]},
                        {_id: [[]]},
                    ],
                    expectDistinctScan: true,
                });
            }
        });

        it("keeps a separate group for scalar undefined on a non-multikey index", function () {
            setupCollection([{a: undefined}, {a: null}, {b: 1}, {a: 7}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: undefined}, {_id: null}, {_id: 7}],
                expectDistinctScan: true,
            });
        });

        it("keeps a separate group for scalar undefined when only another field is multikey", function () {
            for (const index of [
                {a: 1, b: 1},
                {a: -1, b: 1},
            ]) {
                setupCollection(
                    [{a: undefined, b: [1, 2]}, {a: null, b: 3}, {b: [4]}, {a: 7, b: 5}],
                    [index],
                );
                assertResultsAndPlan({
                    pipeline: [unwindA, groupOnA],
                    expectedResults: [{_id: undefined}, {_id: null}, {_id: 7}],
                    expectDistinctScan: true,
                });
            }
        });

        it("keeps a group for every BSON type when normalizing undefined key to null", function () {
            const oid = ObjectId();
            const date = new Date();
            const docs = [
                {a: MinKey},
                {b: 1},
                {a: null},
                {a: []},
                // The scalar undefined merges into the null group, see 'known deviations'.
                {a: undefined},
                {a: NumberInt(1)},
                {a: NumberLong(2)},
                {a: 3.5},
                {a: NumberDecimal("4.4")},
                {a: "s"},
                {a: {x: 1}},
                {a: [[7]]},
                {a: BinData(0, "aGVsbG8=")},
                {a: oid},
                {a: true},
                {a: date},
                {a: Timestamp(1, 2)},
                {a: /re/},
                {a: Code("function() {}")},
                {a: MaxKey},
            ];
            const expectedResults = [
                {_id: MinKey},
                {_id: null},
                {_id: 1},
                {_id: NumberLong(2)},
                {_id: 3.5},
                {_id: NumberDecimal("4.4")},
                {_id: "s"},
                {_id: {x: 1}},
                {_id: [7]},
                {_id: BinData(0, "aGVsbG8=")},
                {_id: oid},
                {_id: true},
                {_id: date},
                {_id: Timestamp(1, 2)},
                {_id: /re/},
                {_id: Code("function() {}")},
                {_id: MaxKey},
            ];
            for (const index of [{a: 1}, {a: -1}]) {
                setupCollection(docs, [index]);
                assertResultsAndPlan({
                    pipeline: [unwindA, groupOnA],
                    expectedResults,
                    expectedControlResults: [...expectedResults, {_id: undefined}],
                    expectDistinctScan: true,
                });
            }
        });
    });

    describe("known deviations", function () {
        it("surfaces top-level undefined elements as null", function () {
            // Nullish BSON values produce the following group keys:
            // - undefined => undefined
            // - [] => null
            // - null => null
            // - missing => null
            //
            // Hence, [] (which gets the same index key as undefined) needs to be treated as null
            // when answering a $unwind+$group query via a distinct scan over a multikey index.
            // This comes at the cost of incorrectly treating a top-level undefined value as null,
            // which is OK because the undefined BSON type is deprecated.
            setupCollection([{a: [undefined]}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: null}],
                expectedControlResults: [{_id: undefined}],
                expectDistinctScan: true,
            });
        });

        // Similar to the above, but with both null and undefined values present.
        it("merges the undefined group into the null group when both are present", function () {
            for (const index of [{a: 1}, {a: -1}]) {
                setupCollection(
                    [{a: [undefined]}, {a: [null, undefined]}, {a: [undefined, undefined]}],
                    [index],
                );
                assertResultsAndPlan({
                    pipeline: [unwindA, groupOnA],
                    expectedResults: [{_id: null}],
                    expectedControlResults: [{_id: null}, {_id: undefined}],
                    expectDistinctScan: true,
                });
            }
        });
    });

    describe("other index types", function () {
        it("does not use a sparse index, which has no entries for missing values", function () {
            setupCollection([{a: [1, 2]}, {b: 9}], [{key: {a: 1}, options: {sparse: true}}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: null}],
                expectDistinctScan: false,
            });
        });

        it("does not use a partial index", function () {
            setupCollection(
                [{a: [1, 2]}, {a: 7}],
                [{key: {a: 1}, options: {partialFilterExpression: {a: {$gt: 0}}}}],
            );
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 7}],
                expectDistinctScan: false,
            });
        });

        it("does not use a hashed index, whose keys are not the values", function () {
            // Hashed indexes cannot index arrays, so the data stays scalar.
            setupCollection([{a: 1}, {a: 2}, {b: 9}], [{a: "hashed"}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: null}],
                expectDistinctScan: false,
            });
        });

        it("uses a compound index with a trailing hashed field", function () {
            // Hashed indexes reject arrays on every field, so only scalar data can exist here.
            setupCollection([{a: 1, b: 1}, {a: 3, b: 2}, {b: 9}], [{a: 1, b: "hashed"}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 3}, {_id: null}],
                expectDistinctScan: true,
            });
        });

        it("does not use a wildcard index, which has no entries for missing values", function () {
            setupCollection([{a: [1, 2]}, {b: 9}], [{"$**": 1}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: null}],
                expectDistinctScan: false,
            });
        });

        it("does not use an index with a non-simple collation", function () {
            setupCollection(
                [{a: ["x", "y"]}, {a: "z"}],
                [{key: {a: 1}, options: {collation: {locale: "fr"}}}],
            );
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: "x"}, {_id: "y"}, {_id: "z"}],
                expectDistinctScan: false,
            });
        });
    });

    describe("ineligible pipelines", function () {
        it("does not rewrite without preserveNullAndEmptyArrays", function () {
            setupCollection([{b: 1}, {a: null}, {a: []}, {a: [null, 1]}, {a: 2}]);
            assertResultsAndPlan({
                pipeline: [{$unwind: "$a"}, groupOnA],
                expectedResults: [{_id: null}, {_id: 1}, {_id: 2}],
                expectDistinctScan: false,
            });
        });

        it("does not rewrite with includeArrayIndex", function () {
            assertResultsAndPlan({
                pipeline: [
                    {
                        $unwind: {
                            path: "$a",
                            preserveNullAndEmptyArrays: true,
                            includeArrayIndex: "idx",
                        },
                    },
                    groupOnA,
                ],
                expectedResults: [{_id: null}, {_id: 1}, {_id: 2}],
                expectDistinctScan: false,
            });
        });

        it("does not rewrite when grouping on a different field than the $unwind", function () {
            setupCollection(
                [
                    {a: [1, 2], b: 1},
                    {a: 3, b: 2},
                ],
                [{a: 1}, {b: 1}],
            );
            assertResultsAndPlan({
                pipeline: [unwindA, {$group: {_id: "$b"}}],
                expectedResults: [{_id: 1}, {_id: 2}],
                expectDistinctScan: false,
            });
        });

        it("does not rewrite with a non-simple collation, since index keys may be collated", function () {
            setupCollection([{a: ["foo", "FOO"]}, {a: "bar"}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA],
                expectedResults: [{_id: "foo"}, {_id: "bar"}],
                expectDistinctScan: false,
                options: {collation: {locale: "en_US", strength: 2}},
            });
        });
    });

    describe("surrounding stages", function () {
        it("leaves stages after the rewritten $unwind and $group unaffected", function () {
            setupCollection([{a: [1, 2, 3]}, {a: [2, 3, 4]}]);
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA, {$sort: {_id: 1}}],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
                expectDistinctScan: true,
            });
            assertResultsAndPlan({
                pipeline: [unwindA, groupOnA, {$sort: {_id: -1}}],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
                expectDistinctScan: true,
            });
        });

        it("does not rewrite when a $sort precedes the $group", function () {
            setupCollection([{a: [1, 2, 3]}, {a: [2, 3, 4]}]);
            assertResultsAndPlan({
                pipeline: [{$sort: {a: 1}}, unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
                expectDistinctScan: false,
            });
            assertResultsAndPlan({
                pipeline: [unwindA, {$sort: {a: 1}}, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
                expectDistinctScan: false,
            });
        });

        it("does not rewrite when a stage between $unwind and $group modifies the field", function () {
            setupCollection([{a: [1, 2]}, {b: 9}]);
            assertResultsAndPlan({
                pipeline: [unwindA, {$addFields: {a: {$ifNull: ["$a", 0]}}}, groupOnA],
                expectedResults: [{_id: 0}, {_id: 1}, {_id: 2}],
                expectDistinctScan: false,
            });
        });
    });

    describe("dotted paths", function () {
        it("does not rewrite for dotted paths with arrays along the prefix", function () {
            // $unwind does not traverse the 'foo' array and passes the document through, while
            // the $group expression collects [[1, 2], 3] and the index holds the keys 1, 2, 3.
            setupCollection([{foo: [{a: [1, 2]}, {a: 3}]}], [{"foo.a": 1}]);
            assertResultsAndPlan({
                pipeline: [
                    {$unwind: {path: "$foo.a", preserveNullAndEmptyArrays: true}},
                    {$group: {_id: "$foo.a"}},
                ],
                expectedResults: [{_id: [[1, 2], 3]}],
                expectDistinctScan: false,
            });
        });

        // TODO SERVER-133204: This could be supported in theory.
        it("does not rewrite dotted paths even when only the leaf is multikey", function () {
            setupCollection([{foo: {a: [1, 2]}}, {foo: {a: 3}}, {foo: null}, {}], [{"foo.a": 1}]);
            assertResultsAndPlan({
                pipeline: [
                    {$unwind: {path: "$foo.a", preserveNullAndEmptyArrays: true}},
                    {$group: {_id: "$foo.a"}},
                ],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: null}],
                expectDistinctScan: false,
            });
        });
    });

    describe("filters", function () {
        // TODO SERVER-133195: We could support this in theory, but currently don't.
        it("does not rewrite a $match on the unwound field after the $unwind", function () {
            setupCollection([{a: [1, 5]}, {a: 2}, {a: [7]}, {a: [[5]]}, {a: []}]);
            assertResultsAndPlan({
                pipeline: [unwindA, {$match: {a: {$gte: 2}}}, groupOnA],
                expectedResults: [{_id: 2}, {_id: 5}, {_id: 7}, {_id: [5]}],
                expectDistinctScan: false,
            });
            assertResultsAndPlan({
                pipeline: [unwindA, {$match: {a: null}}, groupOnA],
                expectedResults: [{_id: null}],
                expectDistinctScan: false,
            });
        });

        it("does not rewrite a $match on the unwound field before the $unwind", function () {
            // TODO SERVER-133195: This could in theory be answered by a distinct scan since 'a'
            // is not multikey, but we don't currently support it.
            setupCollection([{a: 1}, {a: 2}, {a: 7}]);
            assertResultsAndPlan({
                pipeline: [{$match: {a: {$gte: 2}}}, unwindA, groupOnA],
                expectedResults: [{_id: 2}, {_id: 7}],
                expectDistinctScan: false,
            });

            setupCollection([{a: [1, 5]}, {a: 2}]);
            assertResultsAndPlan({
                pipeline: [{$match: {a: {$gte: 2}}}, unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 5}],
                expectDistinctScan: false,
            });
        });

        // TODO SERVER-133195: A single point interval on the prefix could be supported.
        it("does not rewrite an equality on a non-array index prefix", function () {
            setupCollection(
                [
                    {b: 1, a: [1, 2]},
                    {b: 1, a: 5},
                    {b: 2, a: [2]},
                ],
                [{b: 1, a: 1}],
            );
            assertResultsAndPlan({
                pipeline: [{$match: {b: 1}}, unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 5}],
                expectDistinctScan: false,
            });
        });

        it("cannot rewrite a range or $in on a non-array index prefix", function () {
            // The scan meets the value 2 under both prefix values and nothing above the rewritten
            // $group deduplicates, so only a single point interval on the prefix can be safe.
            assertResultsAndPlan({
                pipeline: [{$match: {b: {$gte: 1}}}, unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 5}],
                expectDistinctScan: false,
            });
            assertResultsAndPlan({
                pipeline: [{$match: {b: {$in: [1, 2]}}}, unwindA, groupOnA],
                expectedResults: [{_id: 1}, {_id: 2}, {_id: 5}],
                expectDistinctScan: false,
            });
        });

        it("does not rewrite a match on a non-array index suffix", function () {
            // The matching entry is intentionally not the first within its band.
            setupCollection(
                [
                    {a: [1, 2], b: 0},
                    {a: [2, 3], b: 1},
                ],
                [{a: 1, b: 1}],
            );
            assertResultsAndPlan({
                pipeline: [{$match: {b: 1}}, unwindA, groupOnA],
                expectedResults: [{_id: 2}, {_id: 3}],
                expectDistinctScan: false,
            });
        });

        it("does not rewrite a match on unindexed fields", function () {
            // The matching entry is intentionally not the first within its band.
            setupCollection([
                {a: [1, 5], b: 2},
                {a: 5, b: 1},
            ]);
            assertResultsAndPlan({
                pipeline: [{$match: {b: 1}}, unwindA, groupOnA],
                expectedResults: [{_id: 5}],
                expectDistinctScan: false,
            });
        });
    });

    describe("accumulators", function () {
        it("does not rewrite with accumulators", function () {
            setupCollection(
                [
                    {a: [1, 2], b: 1},
                    {a: [2, 3], b: 2},
                ],
                [{a: 1, b: 1}],
            );
            assertResultsAndPlan({
                pipeline: [unwindA, {$group: {_id: "$a", count: {$sum: 1}}}],
                expectedResults: [
                    {_id: 1, count: 1},
                    {_id: 2, count: 2},
                    {_id: 3, count: 1},
                ],
                expectDistinctScan: false,
            });
        });

        // TODO SERVER-133206: Some of the $first, $last, $top, and $bottom cases below could
        // probably be supported.
        it("does not rewrite $first and $top with a sort that matches the index", function () {
            setupCollection(
                [
                    {a: [1, 2], b: 10},
                    {a: [2, 3], b: 20},
                    {a: [], b: 5},
                ],
                [{a: 1, b: 1}],
            );
            assertResultsAndPlan({
                pipeline: [
                    unwindA,
                    {$sort: {a: 1, b: 1}},
                    {$group: {_id: "$a", r: {$first: "$b"}}},
                ],
                expectedResults: [
                    {_id: null, r: 5},
                    {_id: 1, r: 10},
                    {_id: 2, r: 10},
                    {_id: 3, r: 20},
                ],
                expectDistinctScan: false,
            });
            assertResultsAndPlan({
                pipeline: [
                    unwindA,
                    {$group: {_id: "$a", r: {$top: {sortBy: {a: 1, b: 1}, output: "$b"}}}},
                ],
                expectedResults: [
                    {_id: null, r: 5},
                    {_id: 1, r: 10},
                    {_id: 2, r: 10},
                    {_id: 3, r: 20},
                ],
                expectDistinctScan: false,
            });
        });

        it("does not rewrite $last and $bottom, which need a backward scan", function () {
            assertResultsAndPlan({
                pipeline: [unwindA, {$sort: {a: 1, b: 1}}, {$group: {_id: "$a", r: {$last: "$b"}}}],
                expectedResults: [
                    {_id: null, r: 5},
                    {_id: 1, r: 10},
                    {_id: 2, r: 20},
                    {_id: 3, r: 20},
                ],
                expectDistinctScan: false,
            });
            assertResultsAndPlan({
                pipeline: [
                    unwindA,
                    {$group: {_id: "$a", r: {$bottom: {sortBy: {a: 1, b: 1}, output: "$b"}}}},
                ],
                expectedResults: [
                    {_id: null, r: 5},
                    {_id: 1, r: 10},
                    {_id: 2, r: 20},
                    {_id: 3, r: 20},
                ],
                expectDistinctScan: false,
            });
        });

        it("does not rewrite $first and $last when the output field is an array", function () {
            setupCollection(
                [
                    {a: 1, b: [0, 100]},
                    {a: 1, b: 50},
                ],
                [{a: 1, b: 1}],
            );
            assertResultsAndPlan({
                pipeline: [
                    unwindA,
                    {$sort: {a: 1, b: 1}},
                    {$group: {_id: "$a", r: {$first: "$b"}}},
                ],
                expectedResults: [{_id: 1, r: [0, 100]}],
                expectDistinctScan: false,
            });
            assertResultsAndPlan({
                pipeline: [unwindA, {$sort: {a: 1, b: 1}}, {$group: {_id: "$a", r: {$last: "$b"}}}],
                expectedResults: [{_id: 1, r: 50}],
                expectDistinctScan: false,
            });
        });
    });
});
