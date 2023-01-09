/**
 * Verify the usage of DISTINCT_SCAN when a regex ending with .* is a prefix of an index.
 *
 * @tags: [
 *  requires_fcv_53,
 *  assumes_read_concern_local,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/analyze_plan.js");     // For getPlanStages.
load("jstests/libs/fixture_helpers.js");  // For numberOfShardsForCollection.

const coll = db.regex_distinct;
coll.drop();

assert.commandWorked(coll.insertMany(
    [{a: "abc", b: "foo"}, {a: "abc", b: "bar"}, {a: "abd", b: "far"}, {a: "aeb", b: "car"}]));

assert.commandWorked(coll.createIndex({a: 1}));
assert.eq(2, coll.distinct("a", {a: {"$regex": "^ab.*"}}).length);
const distinctScanStages =
    getPlanStages(coll.explain().distinct("a", {a: {"$regex": "^ab.*"}}), "DISTINCT_SCAN");

assert.eq(distinctScanStages.length, FixtureHelpers.numberOfShardsForCollection(coll));
})();
