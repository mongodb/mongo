/**
 * Test wildcard index support when the query contains duplicate predicates.
 * @tags: [
 *   assumes_read_concern_local,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getPlanStages.

const coll = db.wildcard_index_dup_predicates;
coll.drop();

const indexWildcard = {
    "$**": 1
};

// Inserts the given document and runs the given query to confirm that:
// (1) query matches the given document
// (2) the winning plan does a wildcard index scan
function assertExpectedDocAnswersWildcardIndexQuery(doc, query, match) {
    coll.drop();
    assert.commandWorked(coll.createIndex(indexWildcard));
    assert.commandWorked(coll.insert(doc));

    // Check that a wildcard index scan is being used to answer query.
    const explain = coll.explain("executionStats").find(query).finish();
    if (!match) {
        assert.eq(0, explain.executionStats.nReturned, explain);
        return;
    }

    // Check that the query returns the document.
    assert.eq(1, explain.executionStats.nReturned, explain);

    // Winning plan uses a wildcard index scan.
    const winningPlan = getWinningPlan(explain.queryPlanner);
    const ixScans = getPlanStages(winningPlan, "IXSCAN");
    assert.gt(ixScans.length, 0, explain);
    ixScans.forEach((ixScan) => assert.eq(ixScan.indexName, "$**_1"));
}

assertExpectedDocAnswersWildcardIndexQuery(
    {a: {b: "foo"}},
    {$and: [{a: {$type: 'object'}}, {a: {$type: 'object'}}, {"a.b": "foo"}]},
    true);

assertExpectedDocAnswersWildcardIndexQuery({a: {b: "foo"}},
                                           {
                                               $and: [
                                                   {$expr: {$gt: ["$a", {$literal: {}}]}},
                                                   {$expr: {$gt: ["$a", {$literal: {}}]}},
                                                   {"a.b": "foo"}
                                               ]
                                           },
                                           true);

assertExpectedDocAnswersWildcardIndexQuery(
    {a: {b: "foo"}}, {$and: [{a: {$gt: {}}}, {a: {$gt: {}}}, {"a.b": "foo"}]}, true);

assertExpectedDocAnswersWildcardIndexQuery(
    {a: {b: "foo"}}, {$and: [{a: {$ne: 3}}, {a: {$ne: 3}}, {"a.b": "foo"}]}, true);

assertExpectedDocAnswersWildcardIndexQuery(
    {a: {b: "foo"}},
    {$and: [{a: {$nin: [3, 4, 5]}}, {a: {$nin: [3, 4, 5]}}, {"a.b": "foo"}]},
    true);

assertExpectedDocAnswersWildcardIndexQuery(
    {a: {b: "foo"}}, {$and: [{a: {$exists: true}}, {a: {$exists: true}}, {"a.b": "foo"}]}, true);

assertExpectedDocAnswersWildcardIndexQuery(
    {a: {b: "foo"}},
    {$and: [{a: {$elemMatch: {$gt: {}}}}, {a: {$elemMatch: {$gt: {}}}}, {"a.b": "foo"}]},
    false);
})();
