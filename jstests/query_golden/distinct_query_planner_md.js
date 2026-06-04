/**
 * Tests that we generate DISTINCT_SCANs for the specific cases treated by the query planner
 * (sort that is required only for distinct scan plans and manual covered distinct scan
 * construction).
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 *   requires_fcv_82
 * ]
 */
import {code, section, subSection} from "jstests/libs/query/pretty_md.js";
import {getWinningPlanFromExplain, normalizePlan} from "jstests/libs/query/analyze_plan.js";
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";

const coll = db[jsTestName()];

section("Sort Pattern Added for $groupByDistinctScan");

subSection("Suitable Index for Sort => Distinct Scan");
coll.drop();
coll.createIndex({a: 1, b: 1});
coll.insertMany([
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 3},
]);
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}}}]);

subSection("Suitable Index for Sort (Inverse Order) => Distinct Scan");
coll.drop();
coll.insertMany([
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 3},
]);
coll.createIndex({a: -1, b: -1});
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}}}]);

subSection("No Suitable Index for Sort => No Distinct Scan and No Blocking Sort");
coll.drop();
coll.createIndex({a: 1});
coll.insertMany([
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 3},
]);
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}}}]);

subSection("Suitable Index for Filter but Not for Sort => No Distinct Scan and No Blocking Sort");
coll.drop();
coll.insertMany([
    {a: 1, b: 4},
    {a: 3, b: 8},
    {a: 5, b: 6},
    {a: 5, b: 7},
    {a: 5, b: 4},
    {a: 6, b: 7},
    {a: 6, b: 8},
    {a: 7, b: 9},
    {a: 7, b: 3},
]);
coll.createIndex({a: 1});
outputAggregationPlanAndResults(coll, [
    {$match: {a: {$gt: 3}}},
    {$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}}},
]);

section("Construction of Distinct Scan when No Sort and No Filter");

subSection("$group Stage with no $sort Stage and with suitable index => DISTINCT_SCAN");
coll.drop();
coll.createIndex({a: 1});
coll.insertMany([
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 3},
]);
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a"}}]);

subSection("$group Stage with no $sort Stage and with no suitable index => No DISTINCT_SCAN");
coll.drop();
coll.createIndex({b: 1, a: 1});
coll.insertMany([
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 3},
]);
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a"}}]);

section("DISTINCT_SCAN Preferred Over a Competing Predicate Index");

subSection("Low-cardinality $group with a competing predicate index => DISTINCT_SCAN");
coll.drop();
coll.createIndex({a: 1});
coll.createIndex({b: 1});
coll.createIndex({a: 1, b: 1});
const docs = [];
for (let a = 0; a < 110; a++) {
    docs.push({a, b: "x"});
    docs.push({a, b: "y"});
}
coll.insertMany(docs);
const pipeline = [
    {$match: {a: {$gte: 0}, b: "x"}},
    {$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: -1}}}}},
];
subSection("Pipeline");
code(tojson(pipeline));
subSection("Winning plan");
code(tojson(normalizePlan(getWinningPlanFromExplain(coll.explain().aggregate(pipeline)))));
