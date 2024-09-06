/**
 * Tests that the aggregation will go through the process of multiplanning when the pipeline can be
 * rewritten for the distinct case.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 * ]
 */
import {outputAggregationPlanAndResults, section, subSection} from "jstests/libs/pretty_md.js";

const coll = db[jsTestName()];

coll.drop();
coll.createIndex({a: 1});
coll.createIndex({b: 1});
coll.createIndex({a: 1, b: 1});
coll.createIndex({a: -1, b: 1});
coll.createIndex({a: 1, b: -1});
coll.createIndex({a: 1, b: 1, c: 1});
coll.createIndex({a: 1, b: 1, d: 1});
coll.createIndex({b: 1, a: 1});
coll.createIndex({b: 1, c: 1});
coll.createIndex({d: 1, c: -1});

section("Only DISTINCT_SCAN candidates considered");
coll.insertMany([
    {_id: 1, a: 4, b: 2, c: 3, d: 4},
    {_id: 2, a: 4, b: 3, c: 6, d: 5},
    {_id: 3, a: 5, b: 4, c: 7, d: 5}
]);
outputAggregationPlanAndResults(
    coll, [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {a: 1, b: -1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);
outputAggregationPlanAndResults(
    coll, [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$c"}}}}]);
outputAggregationPlanAndResults(
    coll, [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: -1, b: -1}, output: "$c"}}}}]);
outputAggregationPlanAndResults(
    coll, [{$group: {_id: "$a", accum: {$bottom: {sortBy: {a: 1, b: -1}, output: "$c"}}}}]);
outputAggregationPlanAndResults(coll, [{$group: {_id: "$_id", accum: {$first: "$b"}}}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {a: 1, b: -1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {a: -1, b: 1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);
// Ensure the planner correctly reverses the DISTINCT_SCAN direction for $first and $top.
outputAggregationPlanAndResults(
    coll, [{$group: {_id: "$d", accum: {$top: {sortBy: {d: -1}, output: "$c"}}}}]);
outputAggregationPlanAndResults(coll,
                                [{$sort: {d: -1}}, {$group: {_id: "$d", accum: {$first: "$c"}}}]);

section("Both DISTINCT_SCAN and non-DISTINCT_SCAN candidates considered");
coll.insertMany([{a: 4, b: 2, c: 3}, {a: 4, b: 3, c: 6}, {a: 5, b: 4, c: 7, d: [1, 2, 3]}]);
subSection("DISTINCT_SCAN selected");
outputAggregationPlanAndResults(
    coll, [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}]);

// TODO SERVER-92469: See if we have to do something to break the tie.
subSection("Multiplanning tie between DISTINCT_SCAN and IXSCAN");
const coll2 = db[jsTestName() + "-2"];
coll2.drop();
const distinctDocs = [];
for (let i = 0; i < 5; i++) {
    distinctDocs.push({a: i, b: -i});
}
coll2.insertMany(distinctDocs);
coll2.createIndex({a: -1, b: 1});
coll2.createIndex({a: 1, b: 1});
outputAggregationPlanAndResults(
    coll2,
    [
        {$match: {a: {$gt: 0}}},
        {$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$b"}}}}
    ],
    true,
    5);

section("No DISTINCT_SCAN candidates considered due to conflicting sort specs");
// The $sort is incompatible with the 'sortBy', so a distinct scan can't provide both sorts.
outputAggregationPlanAndResults(coll, [
    {$sort: {a: 1, b: 1}},
    {$group: {_id: "$a", accum: {$top: {sortBy: {b: 1, a: 1}, output: "$c"}}}}
]);
// This query could (and after SERVER-94369, possibly will) be answered by a forward distinct scan
// on a_1_b_1.
outputAggregationPlanAndResults(coll, [
    {$sort: {a: 1, b: 1}},
    {$group: {_id: "$a", accum: {$bottom: {sortBy: {a: -1, b: -1}, output: "$c"}}}}
]);

section("No DISTINCT_SCAN candidates considered due to multikey index");
coll.insertMany([{a: 4, b: 2, c: 3}, {a: 4, b: 3, c: 6}, {a: [1, 2, 3], b: 4, c: 7, d: 5}]);
outputAggregationPlanAndResults(
    coll, [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}]);
subSection("No available indexes");
outputAggregationPlanAndResults(
    coll, [{$group: {_id: "$a", accum: {$top: {sortBy: {a: 1, b: 1}, output: "$b"}}}}]);
