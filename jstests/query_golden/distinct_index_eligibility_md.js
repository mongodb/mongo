/**
 * Tests that we generate DISTINCT_SCANs just for eligible indexes.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 *   requires_fcv_82
 * ]
 */
import {section, subSection} from "jstests/libs/pretty_md.js";
import {outputAggregationPlanAndResults, outputDistinctPlanAndResults} from "jstests/libs/query/golden_test_utils.js";

const coll = db[jsTestName()];

section("Distinct Field part of the Index Key Pattern");

subSection("flip && multikey => no DISTINCT_SCAN");
coll.drop();
coll.insert({a: [1, 2, 3], b: 5});
coll.createIndex({a: 1, b: 1});
outputAggregationPlanAndResults(coll, [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);

subSection("flip && !multikey => DISTINCT_SCAN");
coll.drop();
coll.insert({a: 1, b: 5});
coll.createIndex({a: 1, b: 1});
outputAggregationPlanAndResults(coll, [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);

subSection("!flip && strict && multikey on distinct field => no DISTINCT_SCAN");
coll.drop();
coll.insert({a: [1, 2, 3], b: 5});
coll.createIndex({a: 1, b: 1});
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}}}]);

subSection("!flip && strict && the index is only multikey on the non-distinct field => DISTINCT_SCAN");
coll.drop();
coll.insert({a: 1, b: [1, 2, 3]});
coll.createIndex({a: 1, b: 1});
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a", accum: {$top: {output: "$b", sortBy: {a: 1, b: 1}}}}}]);

subSection("!flip && !strict && !multikey on distinct field => DISTINCT_SCAN");
coll.drop();
coll.insert({a: 1, b: [1, 2, 3]});
coll.createIndex({a: 1, b: 1});
outputDistinctPlanAndResults(coll, "a", {a: {$gt: 3}});

subSection("strict && sparse index => no DISTINCT_SCAN");
coll.drop();
coll.insert({b: 5});
coll.createIndex({a: 1}, {sparse: true});
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a"}}]);

subSection("strict (with accum) && sparse index => no DISTINCT_SCAN");
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a", accum: {$last: "$b"}}}]);

subSection("strict (with sort) && sparse index => no DISTINCT_SCAN");
outputAggregationPlanAndResults(coll, [{$sort: {a: 1}}, {$group: {_id: "$a"}}]);

subSection("strict && sparse index && alternative compound index => DISTINCT_SCAN on compound index");
coll.createIndex({a: 1, b: 1});
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a"}}]);

subSection("strict (with accum) && sparse index && alternative compound index => DISTINCT_SCAN on compound index");
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a", accum: {$last: "$b"}}}]);

subSection("strict (with sort) && sparse index && alternative compound index => DISTINCT_SCAN on compound index");
outputAggregationPlanAndResults(coll, [{$sort: {a: 1}}, {$group: {_id: "$a"}}]);

subSection("strict (with sort and accum) && sparse index => no DISTINCT_SCAN");
coll.dropIndex({a: 1});
coll.dropIndex({a: 1, b: 1});
coll.createIndex({a: 1, b: 1}, {sparse: true});
outputAggregationPlanAndResults(coll, [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);

subSection(
    "strict (with sort and accum) && sparse index && alternative compound index => DISTINCT_SCAN on compound index",
);
coll.createIndex({a: 1, b: 1, c: 1});
outputAggregationPlanAndResults(coll, [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$last: "$b"}}}]);

subSection("!strict && sparse index => DISTINCT_SCAN");
coll.drop();
coll.insert({b: 5});
coll.createIndex({a: 1}, {sparse: true});
outputDistinctPlanAndResults(coll, "a");

section("Distinct Field not part of the Index Key Pattern");

subSection("wildcard && covered projection => DISTINCT_SCAN");
coll.drop();
coll.insertMany([{a: 1}, {a: 2}, {a: "3"}, {b: 5}, {b: 7}]);
coll.createIndex({"$**": 1});
outputDistinctPlanAndResults(coll, "a", {a: {$lt: 3}});

subSection("!wildcard => no DISTINCT_SCAN");
coll.drop();
coll.insertMany([{a: 1}, {a: 2}, {a: "3"}, {b: 5}, {b: 7}]);
coll.createIndex({b: 1});
outputDistinctPlanAndResults(coll, "a", {a: {$lt: 3}});

subSection("wildcard && !covered projection => no DISTINCT_SCAN");
coll.drop();
coll.insertMany([{a: 1}, {a: 2}, {a: "3"}, {a: 4, b: 3}, {b: 7}]);
coll.createIndex({"$**": 1});
outputDistinctPlanAndResults(coll, "a", {b: {$lt: 5}});
