// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_local,
// ]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq().
load("jstests/libs/analyze_plan.js");         // For getPlanStages().
load("jstests/libs/feature_flag_util.js");    // For "FeatureFlagUtil"
load("jstests/libs/fixture_helpers.js");      // For numberOfShardsForCollection().

// TODO SERVER-68303: Remove the feature flag and update corresponding tests.
const allowCompoundWildcardIndexes =
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "CompoundWildcardIndexes");

const coll = db.wildcard_nonblocking_sort;
coll.drop();

assert.commandWorked(coll.createIndex({"$**": 1}, {wildcardProjection: {"excludedField": 0}}));

for (let i = 0; i < 50; i++) {
    assert.commandWorked(coll.insert({a: i, b: -i, x: [123], excludedField: i}));
}

function checkQueryHasSameResultsWhenUsingIdIndex(query, sort, projection) {
    const l = coll.find(query, projection).sort(sort).toArray();
    const r = coll.find(query, projection).sort(sort).hint({$natural: 1}).toArray();
    assert(arrayEq(l, r));
}

function checkQueryUsesSortTypeAndGetsCorrectResults(
    query, sort, projection, isBlocking, isCompound = false) {
    const explain = assert.commandWorked(coll.find(query, projection).sort(sort).explain());
    const plan = getWinningPlan(explain.queryPlanner);

    const ixScans = getPlanStages(plan, "IXSCAN");
    const sorts = getPlanStages(plan, "SORT");

    if (isBlocking) {
        assert.eq(sorts.length, FixtureHelpers.numberOfShardsForCollection(coll), explain);
        assert.eq(sorts[0].sortPattern, sort, explain);

        // A blocking sort may or may not use the index, so we don't check the length of
        // 'ixScans'.
    } else {
        assert.eq(sorts.length, 0, explain);
        assert.eq(ixScans.length, FixtureHelpers.numberOfShardsForCollection(coll), explain);

        const sortKey = Object.keys(sort)[0];
        if (isCompound) {
            assert.docEq({$_path: 1, [sortKey]: 1, excludedField: 1}, ixScans[0].keyPattern);
        } else {
            assert.docEq({$_path: 1, [sortKey]: 1}, ixScans[0].keyPattern);
        }
    }

    checkQueryHasSameResultsWhenUsingIdIndex(query, sort, projection);
}

function checkQueryUsesNonBlockingSortAndGetsCorrectResults(query, sort, projection, isCompound) {
    checkQueryUsesSortTypeAndGetsCorrectResults(query, sort, projection, false, isCompound);
}

function checkQueryUsesBlockingSortAndGetsCorrectResults(query, sort, projection) {
    checkQueryUsesSortTypeAndGetsCorrectResults(query, sort, projection, true);
}

function runSortTests(dir, proj, isCompound = false) {
    // Test that the $** index can provide a non-blocking sort where appropriate.
    checkQueryUsesNonBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: dir}, proj, isCompound);
    checkQueryUsesNonBlockingSortAndGetsCorrectResults(
        {a: {$gte: 0}, x: 123}, {a: dir}, proj, isCompound);

    // Test that the $** index can produce a solution with a blocking sort where appropriate.
    checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: dir, b: dir}, proj);
    checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: dir, b: -dir}, proj);
    checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: -dir, b: dir}, proj);
    checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$exists: true}}, {a: dir}, proj);
    checkQueryUsesBlockingSortAndGetsCorrectResults({}, {a: dir}, proj);

    // Test sorted queries on a field that is excluded by the $** index's wildcardProjection.
    checkQueryUsesSortTypeAndGetsCorrectResults(
        {a: {$gte: 0}, excludedField: {$gte: 0}},
        {a: dir, excludedField: dir},
        proj,
        !isCompound,  // A compound index can yield a non-blocking sort here.
        isCompound);
    checkQueryUsesBlockingSortAndGetsCorrectResults(
        {excludedField: {$gte: 0}}, {a: dir, excludedField: dir}, proj);

    // Test sorted queries on a multikey field, with and without $elemMatch.
    checkQueryUsesBlockingSortAndGetsCorrectResults({x: 123}, {a: dir}, proj);
    checkQueryUsesBlockingSortAndGetsCorrectResults({x: {$elemMatch: {$eq: 123}}}, {x: dir}, proj);
    checkQueryUsesBlockingSortAndGetsCorrectResults({x: {$elemMatch: {$eq: 123}}}, {a: dir}, proj);
}

// Run each test for both ascending and descending sorts, with and without a projection.
for (const dir of [1, -1]) {
    for (const proj of [{}, {_id: 0, a: 1}]) {
        runSortTests(dir, proj);
    }
}

// Repeat tests for compound wildcard indexes.
if (allowCompoundWildcardIndexes) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(
        coll.createIndex({"$**": 1, excludedField: 1}, {wildcardProjection: {"excludedField": 0}}));

    for (const dir of [1, -1]) {
        for (const proj of [{}, {_id: 0, a: 1}]) {
            runSortTests(dir, proj, {a: dir, excludedField: dir}, true /* isCompound */);
        }
    }
}
})();
