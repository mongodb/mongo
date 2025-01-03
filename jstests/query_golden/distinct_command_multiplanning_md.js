/**
 * Tests that the distinct command will go through the process of multiplanning.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 * ]
 */
import {section, subSection} from "jstests/libs/pretty_md.js";
import {outputDistinctPlanAndResults} from "jstests/libs/query/golden_test_utils.js";

const coll = db[jsTestName()];

section("No DISTINCT_SCAN candidate considered");
coll.drop();
coll.createIndex({x: 1});
coll.createIndex({x: -1});
coll.createIndex({y: 1, x: 1});
coll.createIndex({z: 1, y: 1});
coll.insertMany([
    {x: 3, y: 7, z: 5},
    {x: 5, y: 5, z: 6},
    {x: 5, y: 4, z: 5},
    {x: 6, y: 3, z: 5},
    {x: 7, y: 8, z: 5},
    {x: 8, y: 3, z: 7},
    {x: 8, y: 3, z: 8},
]);
outputDistinctPlanAndResults(coll, "x", {x: {$gt: 3}, z: 5});

coll.insertMany([{x: [1, 2, 3]}, {x: [3, 4, 5]}]);

subSection("No DISTINCT_SCAN candidate considered due to multikeyness");
outputDistinctPlanAndResults(coll, "x", {x: 3});
outputDistinctPlanAndResults(coll, "x", {x: {$gt: 3}, z: 5});

subSection("Only DISTINCT_SCAN candidates considered despite multikeyness");
outputDistinctPlanAndResults(coll, "x");

section("Only DISTINCT_SCAN candidates considered");
coll.drop();
coll.createIndex({x: 1, y: 1});
coll.createIndex({x: -1, y: 1});
coll.createIndex({y: 1, x: 1});
coll.createIndex({x: 1, z: 1, y: 1});
coll.insertMany([
    {x: 3, y: 5, z: 7},
    {x: 5, y: 6, z: 5},
    {x: 5, y: 5, z: 4},
    {x: 6, y: 5, z: 3},
    {x: 7, y: 5, z: 8},
    {x: 8, y: 7, z: 3},
    {x: 8, y: 8, z: 3},
]);
outputDistinctPlanAndResults(coll, "x");
outputDistinctPlanAndResults(coll, "x", {x: 3});
outputDistinctPlanAndResults(coll, "x", {x: {$gt: 3}, y: 5});

section("Prefer DISTINCT_SCAN for many duplicate values in the collection");
coll.drop();
for (let i = 0; i < 100; ++i)
    coll.insert({x: i % 2, y: i + 100, z: i + 200});
coll.createIndex({x: 1});
coll.createIndex({x: 1, y: 1});
coll.createIndex({y: 1, z: 1});
outputDistinctPlanAndResults(coll, "x", {x: {$gt: -1}, y: {$lt: 250}});

section("Prefer FETCH + filter + IXSCAN for more selective predicate on y");
coll.drop();
for (let i = 0; i < 100; ++i)
    coll.insert({x: i, y: i + 100, z: i + 200});
coll.createIndex({x: 1});
coll.createIndex({x: 1, y: 1});
coll.createIndex({y: 1, z: 1});
outputDistinctPlanAndResults(coll, "x", {x: {$gt: -1}, y: {$lt: 105}});

section("Use hinted DISTINCT_SCAN");
coll.drop();
coll.createIndex({x: 1, y: 1});
coll.createIndex({y: 1, x: 1});
coll.createIndex({x: 1, z: 1, y: 1});
coll.insertMany([
    {x: 3, y: 5, z: 7},
    {x: 5, y: 6, z: 5},
    {x: 5, y: 5, z: 4},
    {x: 6, y: 5, z: 3},
    {x: 7, y: 5, z: 8},
    {x: 8, y: 7, z: 3},
    {x: 8, y: 8, z: 3},
]);
outputDistinctPlanAndResults(coll, "x", {x: {$gt: 3}, y: 5}, {hint: {x: 1, y: 1}});

section("Use hinted IXSCAN, even with preferable DISTINCT_SCAN");
coll.drop();
for (let i = 0; i < 100; ++i)
    coll.insert({x: i % 2, y: i + 100, z: i + 200});
coll.createIndex({x: 1});
coll.createIndex({x: 1, y: 1});
coll.createIndex({y: 1, z: 1});
outputDistinctPlanAndResults(coll, "x", {x: {$gt: -1}, y: {$lt: 250}}, {hint: {x: 1}});

section("Use hinted COLLSCAN, even with preferable DISTINCT_SCAN");
outputDistinctPlanAndResults(coll, "x", {x: {$gt: -1}, y: {$lt: 250}}, {hint: {$natural: 1}});

section("Use hinted DISTINCT_SCAN, even with no duplicate values");
coll.drop();
for (let i = 0; i < 100; ++i)
    coll.insert({x: i, y: i + 100, z: i + 200});
coll.createIndex({x: 1});
coll.createIndex({x: 1, y: 1});
coll.createIndex({y: 1, z: 1});
outputDistinctPlanAndResults(coll, "x", {x: {$gt: -1}, y: {$lt: 105}}, {hint: {x: 1, y: 1}});
