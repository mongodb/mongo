/**
 * Tests that a {$**: 1} index can coexist with a {$**: 'text'} index in the same collection.
 * @tags: [
 *   assumes_balancer_off,
 *   assumes_read_concern_local,
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.
load("jstests/libs/analyze_plan.js");         // For getPlanStages and planHasStage.
load("jstests/libs/feature_flag_util.js");    // For "FeatureFlagUtil"
load("jstests/libs/fixture_helpers.js");      // For isMongos.

const assertArrayEq = (l, r) => assert(arrayEq(l, r), tojson(l) + " != " + tojson(r));

const coll = db.wildcard_and_text_indexes;
coll.drop();

// TODO SERVER-68303: Remove the feature flag and update corresponding tests.
const allowCompoundWildcardIndexes =
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "CompoundWildcardIndexes");
const compoundPattern = {
    "pre": 1,
    "$**": -1
};

// Runs a single wildcard query test, confirming that an indexed solution exists, that the $**
// index on the given 'expectedPath' was used to answer the query, and that the results are
// identical to those obtained via COLLSCAN.
function assertWildcardQuery(query, expectedPath, isCompound) {
    if (allowCompoundWildcardIndexes && !isCompound) {
        // Hide the compound wildcard index to make sure the single-field wildcard index is used.
        assert.commandWorked(coll.hideIndex(compoundPattern));
    } else if (allowCompoundWildcardIndexes && isCompound) {
        // Hide the regular wildcard index to make sure the compound wildcard index is used.
        assert.commandWorked(coll.hideIndex({"$**": 1}));
    }

    // Explain the query, and determine whether an indexed solution is available.
    const explainOutput = coll.find(query).explain("executionStats");
    const ixScans = getPlanStages(getWinningPlan(explainOutput.queryPlanner), "IXSCAN");
    // Verify that the winning plan uses the $** index with the expected path.
    assert.eq(ixScans.length, FixtureHelpers.numberOfShardsForCollection(coll));
    for (const key in expectedPath) {
        assert(ixScans[0].keyPattern.hasOwnProperty(key), explainOutput);
        assert.eq(expectedPath[key], ixScans[0].keyPattern[key], explainOutput);
    }
    // Verify that the results obtained from the $** index are identical to a COLLSCAN.
    assertArrayEq(coll.find(query).toArray(), coll.find(query).hint({$natural: 1}).toArray());

    if (allowCompoundWildcardIndexes && !isCompound) {
        assert.commandWorked(coll.unhideIndex(compoundPattern));
    } else if (allowCompoundWildcardIndexes && isCompound) {
        assert.commandWorked(coll.unhideIndex({"$**": 1}));
    }
}

// Insert documents containing the field '_fts', which is reserved when using a $text index.
assert.commandWorked(coll.insertMany([
    {_id: 1, a: 1, _fts: 1, textToSearch: "banana"},
    {_id: 2, a: 1, _fts: 2, textToSearch: "bananas"},
    {_id: 3, a: 1, _fts: 3, pre: 1},
    {_id: 4, a: 1, _fts: 10, pre: 1},
    {_id: 5, a: 1, _fts: 10, pre: 2}
]));

// Build a wildcard index, and verify that it can be used to query for the field '_fts'.
assert.commandWorked(coll.createIndex({"$**": 1}));

if (allowCompoundWildcardIndexes) {
    // Build a compound wildcard index, and verify that it can be used to query for the field '_fts'
    // and a regular field.
    assert.commandWorked(coll.createIndex(compoundPattern, {'wildcardProjection': {pre: 0}}));
}

assertWildcardQuery({_fts: {$gt: 0, $lt: 4}}, {'_fts': 1}, false /* isCompound */);
if (allowCompoundWildcardIndexes) {
    // The expanded CWI key pattern shouldn't have '_fts'. The query is a $and query and 'pre' field
    // is the prefix of the CWI, so it's basically a query on the non-wildcard prefix field of a
    // CWI. The only eligible expanded CWI is with key pattern {"pre": 1, "$_path": 1}.
    assertWildcardQuery({_fts: 10, pre: 1}, {'pre': 1, '$_path': 1}, true /* isCompound */);
}

// Perform the tests below for simple and compound $text indexes.
for (let textIndex of [{'$**': 'text'}, {a: 1, '$**': 'text'}]) {
    // Build the appropriate text index.
    assert.commandWorked(coll.createIndex(textIndex, {name: "textIndex"}));

    // Confirm that the wildcard index can still be used to query for the '_fts' field outside of
    // $text queries.
    assertWildcardQuery({_fts: {$gt: 0, $lt: 4}}, {'_fts': 1}, false /* isCompound */);
    if (allowCompoundWildcardIndexes) {
        assertWildcardQuery({_fts: 10, pre: 1}, {'pre': 1, '$_path': 1}, true /* isCompound */);
    }

    // Confirm that $** does not generate a candidate plan for $text search, including cases
    // when the query filter contains a compound field in the $text index.
    const textQuery = Object.assign(textIndex.a ? {a: 1} : {}, {$text: {$search: 'banana'}});
    let explainOut = assert.commandWorked(coll.find(textQuery).explain("executionStats"));
    assert(planHasStage(coll.getDB(), getWinningPlan(explainOut.queryPlanner), "TEXT_MATCH"));
    assert.eq(getRejectedPlans(explainOut).length, 0);
    assert.eq(explainOut.executionStats.nReturned, 2);

    // Confirm that $** does not generate a candidate plan for $text search, including cases
    // where the query filter contains a field which is not present in the text index.
    explainOut = assert.commandWorked(
        coll.find(Object.assign({_fts: {$gt: 0, $lt: 4}}, textQuery)).explain("executionStats"));
    assert(planHasStage(coll.getDB(), getWinningPlan(explainOut.queryPlanner), "TEXT_MATCH"));
    assert.eq(getRejectedPlans(explainOut).length, 0);
    assert.eq(explainOut.executionStats.nReturned, 2);

    // Confirm that the $** index can be used alongside a $text predicate in an $or.
    explainOut =
        assert.commandWorked(coll.find({$or: [{_fts: 3}, textQuery]}).explain("executionStats"));
    assert.eq(explainOut.executionStats.nReturned, 3);

    const textOrWildcard = getPlanStages(getWinningPlan(explainOut.queryPlanner), "OR").shift();
    assert.eq(textOrWildcard.inputStages.length, 2);
    const textBranch = (textOrWildcard.inputStages[0].stage === "TEXT_MATCH" ? 0 : 1);
    const wildcardBranch = (textBranch + 1) % 2;
    assert.eq(textOrWildcard.inputStages[textBranch].stage, "TEXT_MATCH");
    assert.eq(textOrWildcard.inputStages[wildcardBranch].stage, "IXSCAN");

    assert.eq(textOrWildcard.inputStages[wildcardBranch].indexName.includes("$**"), true);

    // Drop the index so that a different text index can be created.
    assert.commandWorked(coll.dropIndex("textIndex"));
}
})();
