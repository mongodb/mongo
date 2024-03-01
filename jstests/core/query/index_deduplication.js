/**
 * Runs a set of scenarios with index pruning (internalQueryPlannerEnableIndexPruning) enabled and
 * disabled to check that we prune correctly and do not remove potentially useful plans.
 *
 * @tags: [
 * # Expected plan structure changes in these cases.
 * assumes_against_mongod_not_mongos,
 * does_not_support_stepdowns,
 * # Implicit indexes would interfere with our expected results.
 * assumes_no_implicit_index_creation,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * # Index deduping not available on earlier FCVs.
 * requires_fcv_80
 * ]
 */
import {
    getPlanStages,
    getQueryPlanner,
    getRejectedPlans,
    getWinningPlan
} from "jstests/libs/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.index_deduplication;
coll.drop();

coll.insert({a: 1, b: 1, c: 1});

const interestingScenarios = [
    // Indexes can be deduped.
    {index1: {a: 1}, index2: {a: 1, b: 1}, find: {a: 1}, dedup: true},
    {index1: {a: 1, b: 1}, index2: {a: 1, c: 1}, find: {a: 1}, dedup: true},
    {index1: {a: 1, b: 1}, index2: {a: 1, b: 1, c: 1}, find: {a: 1, b: 1}, dedup: true},
    {index1: {a: 1, b: 1, c: 1}, index2: {a: 1, b: 1, c: 1, d: 1}, find: {a: 1, c: 1}, dedup: true},
    // Both indexes can cover projection/sort.
    // TODO SERVER-86639 In the future we can dedup this to just the {a: 1} index since it's
    // shorter. But for now we're being safe and not deduping.
    {index1: {a: 1, b: 1}, index2: {a: 1}, find: {}, project: {_id: 0, a: 1}, dedup: false},
    {index1: {a: 1, b: 1}, index2: {a: 1}, find: {}, sort: {a: 1}, dedup: false},
    // The longer index can cover and the shorter index cannot. This would be tricky case to dedup
    // and may be unsafe, so we do nothing.
    {
        index1: {a: 1, b: 1, c: 1},
        index2: {a: 1},
        find: {},
        project: {_id: 0, a: 1, c: 1},
        dedup: false
    },
    // One index can cover projection/sort.
    {
        index1: {a: 1, b: 1},
        index2: {a: 1},
        find: {a: 1},
        project: {_id: 0, a: 1, b: 1},
        dedup: false
    },
    {index1: {a: 1, b: 1}, index2: {a: 1}, find: {}, sort: {a: 1, b: 1}, dedup: false},
    // TODO SERVER-86639 for now indexes can't be deduped because of different sort order on the
    // index. It seems safe to dedup indexes with different sort orders in some scenarios.
    {index1: {a: 1, b: 1}, index2: {a: -1}, find: {a: 1}, dedup: false},
    {index1: {a: 1, b: 1}, index2: {a: -1, c: -1}, find: {a: 1}, dedup: false},
    {index1: {a: 1, b: 1}, index2: {a: 1, b: -1, c: 1}, find: {a: 1, b: 1}, dedup: false},
    {
        index1: {a: 1, b: 1, c: 1},
        index2: {a: -1, b: 1, c: -1, d: 1},
        find: {a: 1, c: 1},
        dedup: false
    },
    // Indexes can't be deduped.
    {index1: {a: 1}, index2: {b: 1}, find: {a: 1}, dedup: false},
    {index1: {a: 1}, index2: {b: 1, a: 1}, find: {a: 1}, dedup: false},
    {index1: {a: 1, b: 1}, index2: {b: 1, a: 1}, find: {a: 1}, dedup: false},
    {index1: {a: 1, b: 1}, index2: {b: 1, a: 1}, find: {a: 1, b: 1}, dedup: false},
];

function getIxscan(explain) {
    return getPlanStages(getWinningPlan(explain.queryPlanner), "IXSCAN");
}

function getExplain(query, hint) {
    let findPortion = coll.find(query.find, query.project);
    if (query.sort) {
        findPortion = findPortion.sort(query.sort);
    }
    if (hint) {
        findPortion = findPortion.hint(hint);
    }
    return findPortion.explain();
}

// Run the given query with index pruning disabled and then enabled, and returns the explains.
function getExplainBeforeAfterPruning(query) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerEnableIndexPruning: 0}));
    const explainNoPruning = getExplain(query);

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerEnableIndexPruning: 1}));
    const explainWithPruning = getExplain(query);
    return {before: explainNoPruning, after: explainWithPruning};
}

// Check for the same number of plans with and without index pruning.
function doesNotDedup(query) {
    const explains = getExplainBeforeAfterPruning(query);
    // Explain should report we didn't prune before or after.
    assert(!getQueryPlanner(explains.before).prunedSimilarIndexes, query);
    assert(!getQueryPlanner(explains.after).prunedSimilarIndexes, query);

    // Number of plans considered is winning plan (1) plus the number of rejected plans.
    const numPlansNoDedup = 1 + getRejectedPlans(explains.before).length;
    // Number of plans considered is winning plan (1) plus the number of rejected plans.
    const numPlansWithDedup = 1 + getRejectedPlans(explains.after).length;

    assert.eq(numPlansNoDedup, numPlansWithDedup, {query: query, explains: explains});
}

// Check for less plans when pruning is enabled, and assert that we see the correct index in the
// winning plans.
function doesDedup(query) {
    const explains = getExplainBeforeAfterPruning(query);
    const testDebugInfo = {query: query, explains: explains};
    // Explain should report we didn't prune before and did prune after.
    assert(!getQueryPlanner(explains.before).prunedSimilarIndexes, testDebugInfo);
    assert(getQueryPlanner(explains.after).prunedSimilarIndexes, testDebugInfo);

    // Number of plans considered is winning plan (1) plus the number of rejected plans.
    const numPlansNoDedup = 1 + getRejectedPlans(explains.before).length;
    const numPlansWithDedup = 1 + getRejectedPlans(explains.after).length;

    assert.gt(numPlansNoDedup, numPlansWithDedup, testDebugInfo);

    // If the indexes are equal length, either could win in the deduplication. If they're different
    // length, we make sure the shorter index won.
    const index1Len = Object.keys(query.index1).length;
    const index2Len = Object.keys(query.index2).length;
    if (index1Len !== index2Len) {
        // The test cases are setup so index1 is always the shorter one.
        assert.lt(index1Len, index2Len, testDebugInfo);

        // With the scenarios we test there wouldn't be any rejected alternatives once the other
        // indexes are pruned.
        assert.eq(getRejectedPlans(explains.after).length, 0, testDebugInfo);

        // Each plan should only have one index scan.
        assert.eq(getIxscan(explains.after).length, 1, testDebugInfo);
        const ixscan = getIxscan(explains.after)[0];
        assert.eq(query.index1, ixscan.keyPattern, testDebugInfo);
    }
}

// Checks that collated, unique, sparse, and non-btree indexes (hashed) are never deduped.
function neverDedupSpecialIndexes() {
    const specialIndexOptions = [{collation: {locale: "fr"}}, {unique: true}, {sparse: true}];
    // Try all of our interesting cases with special index options, and test for non-deduping.
    for (const indexOption of specialIndexOptions) {
        for (const setup of interestingScenarios) {
            coll.dropIndexes();
            assert.commandWorked(coll.createIndex(setup.index1, indexOption));
            assert.commandWorked(coll.createIndex(setup.index2));
            doesNotDedup(setup);

            // Same scenario but the other index has the special options.
            coll.dropIndexes();
            assert.commandWorked(coll.createIndex(setup.index2, indexOption));
            assert.commandWorked(coll.createIndex(setup.index1));
            doesNotDedup(setup);
        }
    }

    // Similar but one of the indexes is hashed.
    for (const setup of interestingScenarios) {
        coll.dropIndexes();
        const index1WithZHashed = Object.assign({z: 'hashed'}, setup.index1);
        assert.commandWorked(coll.createIndex(index1WithZHashed));
        assert.commandWorked(coll.createIndex(setup.index2));
        doesNotDedup(setup);

        // Same scenario but the other index has the special options.
        coll.dropIndexes();
        const index2WithZHashed = Object.assign({z: 'hashed'}, setup.index2);
        assert.commandWorked(coll.createIndex(index2WithZHashed));
        assert.commandWorked(coll.createIndex(setup.index1));
        doesNotDedup(setup);
    }
}

// Check that we dedup as expected with regular indexes.
function dedupRegularIndexes() {
    for (const setup of interestingScenarios) {
        coll.dropIndexes();
        assert.commandWorked(coll.createIndex(setup.index1));
        assert.commandWorked(coll.createIndex(setup.index2));
        // TODO SERVER-86639 we can handle more sharded deduplication cases if we analyze the shard
        // key.
        if (setup.dedup && !FixtureHelpers.isSharded(coll)) {
            doesDedup(setup);
        } else {
            doesNotDedup(setup);
        }
    }
}

// Check that we can hint indexes that we would usually be deduped.
function canHintDedupedIndex() {
    for (const setup of interestingScenarios) {
        coll.dropIndexes();
        assert.commandWorked(coll.createIndex(setup.index1));
        assert.commandWorked(coll.createIndex(setup.index2));

        // Try hinting both the deduped index and the index that stays.
        for (const hintedIndex of [setup.index1, setup.index2]) {
            const explain = getExplain(setup, hintedIndex, setup.dedup);
            const ixscan = getIxscan(explain)[0];
            assert.eq(hintedIndex, ixscan.keyPattern, {setup: setup, hinted: hintedIndex});
        }
    }
}

// Use a common customer pattern and make sure deduping multiple indexes for a single query works
// properly.
function dedupMultipleIndexes() {
    coll.dropIndexes();
    // All of these should be deduped, leaving only {a: 1, b:1}.
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1, d: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1, x: 1, y: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1, x: 1, y: 1, z: 1}));

    let explain = coll.find({a: 1, b: 1}).explain();
    assert.eq(getRejectedPlans(explain).length, 0, explain);
    assert(getQueryPlanner(explain).prunedSimilarIndexes, explain);

    // SERVER-86639 For sorts we don't dedup yet, but potentially could.
    explain = coll.find().sort({a: 1, b: 1}).explain();
    assert.eq(getRejectedPlans(explain).length, 4, explain);
    assert(!getQueryPlanner(explain).prunedSimilarIndexes, explain);
}

neverDedupSpecialIndexes();
dedupRegularIndexes();
canHintDedupedIndex();
dedupMultipleIndexes();
