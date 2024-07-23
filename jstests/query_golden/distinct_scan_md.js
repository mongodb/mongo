/**
 * Tests the results and explain for distinct() and aggregation queries which might be able
 * to leverage a DISTINCT_SCAN.
 */

import {
    outputAggregationPlanAndResults,
    outputDistinctPlanAndResults,
    section
} from "jstests/libs/pretty_md.js";

const coll = db[jsTestName()];
coll.drop();
coll.insertMany([
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 3},
    {a: 2, b: 4},
    {a: 3, b: 7},
    {a: 4, b: 3},
    {a: 4, b: 2},
    {a: 7, b: 1},
]);

section("distinct() without index");
outputDistinctPlanAndResults(coll, "a");

section("Aggregation without index");
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a"}}]);

coll.createIndex({a: 1});
section("distinct() with index on 'a'");
outputDistinctPlanAndResults(coll, "a");

section("Aggregation on $group with _id field 'a' with index on 'a'");
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a"}}]);

coll.createIndex({b: 1});
coll.createIndex({a: 1, b: 1});
section("distinct() with multiple choices for index");
outputDistinctPlanAndResults(coll, "a");

section("Aggregation with multiple choices for index");
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a", firstField: {$first: "$b"}}}]);

section("distinct() with filter on 'a' with available indexes");
outputDistinctPlanAndResults(coll, "a", {"a": {$lte: 3}});

section("Aggregation with filter on 'a' with available indexes");
outputAggregationPlanAndResults(coll, [{$match: {"a": {$lte: 3}}}, {$group: {_id: "$a"}}]);

section("distinct() with filter on 'b' with available indexes");
outputDistinctPlanAndResults(coll, "a", {"b": 3});

section("Aggregation with filter on 'b' with available indexes");
outputAggregationPlanAndResults(coll, [{$match: {"b": 3}}, {$group: {_id: "$a"}}]);
