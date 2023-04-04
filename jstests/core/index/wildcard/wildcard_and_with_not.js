/**
 * Validate that we obtain the correct results from an $and query with one predicate eligible for a
 * FETCH + IXSCAN + FILTER on a compound wildcard index, and another predicate that cannot be
 * answered by the index.
 *
 * @tags: [
 *   # We may choose a different plan if other indexes are created, which would break the test.
 *   assumes_no_implicit_index_creation,
 *   assumes_read_concern_local,
 *   does_not_support_stepdowns,
 *   # "Explain for the aggregate command cannot run within a multi-document transaction"
 *   does_not_support_transactions,
 *   featureFlagCompoundWildcardIndexes,
 *   requires_fcv_70,
 * ]
 */

(function() {
'use strict';

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/analyze_plan.js");         // For getWinningPlan(), getAggPlanStages().
load("jstests/libs/fixture_helpers.js");      // For numberOfShardsForCollection().

const documentList = [
    {
        "_id": 1328,
        "obj": {
            "_id": 1334,
            "date": null,
            "obj": {
                "_id": 1335,
                "str": "strategize",
                "date": ISODate("2019-03-12T19:21:41.618Z"),
                "obj": {}
            },
        }
    },
    {
        "_id": 1378,
        "obj": {"_id": 1386, "date": null, "obj": null},
    },
    {
        "_id": 1403,
        "obj": {
            "_id": 1404,
            "str": "payment recontextualize",
            "date": ISODate("2019-01-31T12:53:44.088Z")
        },
    },
];

// $graphLookup issues the query in 'testLargerMatch' to the foreign collection, which is why we
// want to test it here. It should result in a COLLSCAN on the foreign collection, though we can
// only validate the results for this case, since the explain does not include index usage on the
// foreign colleciton.
const testGraphLookup = [
    {$graphLookup: {
        from: "that",
        startWith: null,
        connectFromField: "obj.obj.obj.obj.obj.num",
        connectToField: "obj.date",
        as: "array",
        restrictSearchWithMatch: {"obj.obj.obj.str": {$not: {$lte: "redundant"}}}
    }}
];

const testLargerMatch = [{
    $match: {
        $and: [
            {"obj.obj.obj.str": {$not: {$lte: "redundant"}}},
            {"obj.date": {$in: [null]}},
            {"obj.date": {$exists: true}},
            {"obj.date": {$not: {$type: "undefined"}}}
        ]
    }
}];

const testSmallerMatch = [{
    $match: {
        $and: [
            {"obj.obj.obj.str": {$not: {$lte: "redundant"}}},
            {"obj.date": {$exists: true}},
        ]
    }
}];

function assertCollScan(explain) {
    const ixScans = getAggPlanStages(explain, "IXSCAN");
    const collScans = getAggPlanStages(explain, "COLLSCAN");
    assert.eq(ixScans.length, 0, explain);
    assert.eq(collScans.length, FixtureHelpers.numberOfShardsForCollection(coll), explain);
}

function ensureCorrectResultsWithAndWithoutPlanning(collName, pipeline, useCollScan) {
    const expected = db[collName].aggregate(pipeline, {hint: {$natural: 1}}).toArray();
    const actual = db[collName].aggregate(pipeline /* No hint! */).toArray();
    if (useCollScan) {
        assertCollScan(db[collName].explain().aggregate(pipeline /* No hint! */));
    }
    assertArrayEq({expected, actual});
}

function testAndMatches(useCollScan) {
    ensureCorrectResultsWithAndWithoutPlanning("this", testGraphLookup, useCollScan);
    ensureCorrectResultsWithAndWithoutPlanning("that", testLargerMatch, useCollScan);
    ensureCorrectResultsWithAndWithoutPlanning("that", testSmallerMatch, useCollScan);
}

db = db.getSiblingDB(jsTestName());

const coll = db.this;
coll.drop();
assert.commandWorked(coll.insert({_id: "whatever"}));

const that = db.that;
that.drop();
assert.commandWorked(that.insert(documentList));

// Create a single-field wildcard index (always ineligible).
assert.commandWorked(that.createIndex({"obj.obj.obj.$**": 1}, {}));
testAndMatches(true /* useCollScan */);

// Create a compound wildcard index with obj.date as a prefix (eligible for IXSCAN + FILTER).
assert.commandWorked(that.dropIndexes());
assert.commandWorked(that.createIndex({"obj.date": 1, "obj.obj.obj.$**": 1}, {}));
// This CWI with non-wildcard prefix fields can provide index scan plans.
testAndMatches(false /* useCollScan */);

// Create a compound wildcard index with obj.date as a suffix (always ineligible).
assert.commandWorked(that.dropIndexes());
assert.commandWorked(that.createIndex({"obj.obj.obj.$**": 1, "obj.date": 1}, {}));
testAndMatches(true /* useCollScan */);
})();
