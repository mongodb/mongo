/**
 * Confirms that explode for sort plans are properly cached and recovered from the plan cache,
 * yielding correct results after the query is auto-parameterized.
 *
 * @tags: [
 *   # Since the plan cache is per-node state, this test assumes that all operations are happening
 *   # against the same mongod.
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged,
 *   does_not_support_stepdowns,
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   assumes_unsharded_collection,
 *   requires_fcv_70,
 *   # Plan cache state is node-local and will not get migrated alongside tenant data.
 *   tenant_migration_incompatible,
 *   # Part of this test does different checks depending on the engine used.  If an implicit index
 *   # is created, the engine use depends on what index is implicitly created. E.g. if a column
 *   # index is implicitly created, the engine used may be different in that passthrough.
 *   assumes_no_implicit_index_creation,
 *   # Plan does not support repeat queries
 *   does_not_support_repeated_reads,
 * ]
 */
import {
    getPlanCacheKeyFromShape,
    getPlanStages,
    getWinningPlan
} from "jstests/libs/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const isSBEEnabled = checkSbeFullyEnabled(db);
const coll = db.explode_for_sort_plan_cache;
coll.drop();

// Create two indexes to ensure the multi-planner kicks in and the query plan gets cached when the
// classic engine is in use.
assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1}));
assert.commandWorked(coll.createIndex({a: -1, b: 1, c: 1, d: 1}));

const sortSpec = {
    c: 1
};

// A helper function to look up a cache entry in the plan cache based on the given filter
// and sort specs.
function getPlanForCacheEntry(query, sort) {
    const keyHash = getPlanCacheKeyFromShape({query: query, sort: sort, collection: coll, db: db});

    const res =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).toArray();
    // We expect exactly one matching cache entry.
    assert.eq(1, res.length, () => tojson(coll.aggregate([{$planCacheStats: {}}]).toArray()));
    return res[0];
}

function assertIsExplodeForSort(query) {
    const explain = coll.find(query).sort(sortSpec).explain();
    const winningPlan = getWinningPlan(explain.queryPlanner);
    const sortMerges = getPlanStages(winningPlan, 'SORT_MERGE');
    assert.eq(sortMerges.length, 1, explain);
}

function assertIsNotExplodeForSort(query) {
    const explain = coll.find(query).sort(sortSpec).explain();
    const winningPlan = getWinningPlan(explain.queryPlanner);
    const sortMerges = getPlanStages(winningPlan, 'SORT_MERGE');
    assert.eq(sortMerges.length, 0, explain);
}

function assertQueryParameterizedCorrectly(
    {query, queryCount, newQuery, newQueryCount, reuseEntry, isExplode = true}) {
    // Clear plan cache to have a fresh test case.
    coll.getPlanCache().clear();

    if (isExplode) {
        assertIsExplodeForSort(query);
    } else {
        assertIsNotExplodeForSort(query);
    }

    // Run the query for the first time to create an inactive plan cache entry.
    assert.eq(queryCount, coll.find(query).sort(sortSpec).itcount());
    let inactiveEntry = getPlanForCacheEntry(query, sortSpec);
    assert.eq(inactiveEntry.isActive, false, inactiveEntry);

    // Run the same query again to activate the cache entry.
    assert.eq(queryCount, coll.find(query).sort(sortSpec).itcount());
    const activeEntry = getPlanForCacheEntry(query, sortSpec);
    assert.eq(activeEntry.isActive, true, activeEntry);
    assert.eq(inactiveEntry.queryHash,
              activeEntry.queryHash,
              `inactive=${tojson(inactiveEntry)}, active=${tojson(activeEntry)}`);
    assert.eq(inactiveEntry.planCacheKey,
              activeEntry.planCacheKey,
              `inactive=${tojson(inactiveEntry)}, active=${tojson(activeEntry)}`);

    // Run the new query and check for cache entry.
    assert.eq(newQueryCount, coll.find(newQuery).sort(sortSpec).itcount());
    if (reuseEntry) {
        const reusedEntry = getPlanForCacheEntry(newQuery, sortSpec);
        assert.eq(reusedEntry.isActive, true, reusedEntry);
        assert.eq(activeEntry.queryHash,
                  reusedEntry.queryHash,
                  `active=${tojson(activeEntry)}, reused=${tojson(reusedEntry)}`);
        assert.eq(activeEntry.planCacheKey,
                  reusedEntry.planCacheKey,
                  `active=${tojson(activeEntry)}, reused=${tojson(reusedEntry)}`);
    } else {
        inactiveEntry = getPlanForCacheEntry(newQuery, sortSpec);
        assert.eq(inactiveEntry.isActive, false, inactiveEntry);
        assert.neq(inactiveEntry.queryHash,
                   activeEntry.queryHash,
                   `inactive=${tojson(inactiveEntry)}, active=${tojson(activeEntry)}`);
        assert.neq(inactiveEntry.planCacheKey,
                   activeEntry.planCacheKey,
                   `inactive=${tojson(inactiveEntry)}, active=${tojson(activeEntry)}`);
    }
}

// Insert documents into the collection in a way that we can deduce query result count by the query
// parameters.
for (let a = 1; a <= 3; a++) {
    for (let aIdx = 0; aIdx < a; aIdx++) {
        for (let b = 1; b <= 3; b++) {
            for (let bIdx = 0; bIdx < b; bIdx++) {
                assert.commandWorked(coll.insert({a, b}));
            }
        }
    }
}

// Query with shape not like point intervals but evaluates to point intervals at runtime will not be
// optimized with exploding for sort.
assertIsNotExplodeForSort({a: {$gte: 1, $lte: 1}, b: {$in: [1, 2]}});

// Changing the $eq predicate value should reuse the plan cache and gives correct results.
assertQueryParameterizedCorrectly({
    query: {a: {$eq: 1}, b: {$in: [1, 2]}},
    queryCount: 3,
    newQuery: {a: {$eq: 2}, b: {$in: [1, 2]}},
    newQueryCount: 6,
    reuseEntry: true,
});

// Changing the $expr-$eq predicate value should not reuse the SBE plan cache because agg expression
// is not parameterized.
assertQueryParameterizedCorrectly({
    query: {$and: [{$expr: {$eq: ["$a", 1]}}, {b: {$in: [1, 2]}}]},
    queryCount: 3,
    newQuery: {$and: [{$expr: {$eq: ["$a", 2]}}, {b: {$in: [1, 2]}}]},
    newQueryCount: 6,
    // The plan cache entry is always reused for the classic engine but never reused for the SBE
    // engine.
    reuseEntry: !isSBEEnabled,
});

// Rewriting the $in predicate with $or should reuse the plan cache and gives correct results.
assertQueryParameterizedCorrectly({
    query: {a: {$eq: 1}, b: {$in: [1, 2]}},
    queryCount: 3,
    newQuery: {$and: [{a: {$eq: 2}}, {$or: [{b: {$eq: 1}}, {b: {$eq: 2}}]}]},
    newQueryCount: 6,
    reuseEntry: true,
});

// Changing the $or predicate value should reuse the plan cache and gives correct results, since $or
// is rewritten as $in.
assertQueryParameterizedCorrectly({
    query: {$and: [{a: {$eq: 1}}, {$or: [{b: {$eq: 1}}, {b: {$eq: 2}}]}]},
    queryCount: 3,
    newQuery: {$and: [{a: {$eq: 1}}, {$or: [{b: {$eq: 1}}, {b: {$eq: 3}}]}]},
    newQueryCount: 4,
    reuseEntry: true,
});

// Changing the $in predicate values but not size should reuse the plan cache and gives correct
// results.
assertQueryParameterizedCorrectly({
    query: {a: {$eq: 1}, b: {$in: [1, 2]}},
    queryCount: 3,
    newQuery: {a: {$eq: 1}, b: {$in: [1, 3]}},
    newQueryCount: 4,
    reuseEntry: true,
});

// Changing the $in predicate size should not reuse the SBE plan cache but still gives correct
// results.
assertQueryParameterizedCorrectly({
    query: {a: {$eq: 1}, b: {$in: [1, 2]}},
    queryCount: 3,
    newQuery: {a: {$eq: 1}, b: {$in: [1, 2, 3]}},
    newQueryCount: 6,
    reuseEntry: !isSBEEnabled,
});

// Special values in the predicate will not be parameterized hence the SBE plan cache will not be
// reused.
for (let specialValue of [null, {x: 1}, [1]]) {
    assertQueryParameterizedCorrectly({
        query: {a: {$eq: 1}, b: {$in: [1, 2]}},
        queryCount: 3,
        newQuery: {a: {$eq: specialValue}, b: {$in: [1, 2]}},
        newQueryCount: 0,
        reuseEntry: !isSBEEnabled,
    });
    assertQueryParameterizedCorrectly({
        query: {a: {$eq: 1}, b: {$in: [1, 2]}},
        queryCount: 3,
        newQuery: {a: {$eq: 1}, b: {$in: [0, specialValue]}},
        newQueryCount: 0,
        reuseEntry: !isSBEEnabled,
    });
}

// Regex in $in predicate will not reuse plan cache.
assertQueryParameterizedCorrectly({
    query: {a: {$eq: 1}, b: {$in: [1, 2]}},
    queryCount: 3,
    newQuery: {a: {$eq: 1}, b: {$in: [0, /a|regex/]}},
    newQueryCount: 0,
    reuseEntry: false,
});

const maxScansToExplode = assert.commandWorked(db.adminCommand(
    {getParameter: 1, internalQueryMaxScansToExplode: 1}))["internalQueryMaxScansToExplode"];

const maxExplodeIn = [];
for (let i = 0; i < maxScansToExplode; ++i) {
    maxExplodeIn.push(i);
}
const tooLargeToExplodeIn = maxExplodeIn.concat([maxScansToExplode]);
const evenLargerIn = tooLargeToExplodeIn.concat([maxScansToExplode + 1]);

// Test that an $in with as many elements as the "max scans to explode" parameter uses explode for
// sort and is parameterized correctly.
assertQueryParameterizedCorrectly({
    query: {a: {$eq: 1}, b: {$in: maxExplodeIn}},
    queryCount: 6,
    newQuery: {a: {$eq: 2}, b: {$in: maxExplodeIn}},
    newQueryCount: 12,
    reuseEntry: true,
});

// Test that in SBE a query with explode for sort optimization will use different plan cache
// entry than the same query with too many scans to explode.
assertQueryParameterizedCorrectly({
    query: {a: {$eq: 1}, b: {$in: maxExplodeIn}},
    queryCount: 6,
    newQuery: {a: {$eq: 1}, b: {$in: tooLargeToExplodeIn}},
    newQueryCount: 6,
    reuseEntry: !isSBEEnabled,
});

// Test that a query with more than "max scans to explode" will use the same plan cache
// entry for any number of elements in $in.
assertQueryParameterizedCorrectly({
    query: {a: {$eq: 1}, b: {$in: tooLargeToExplodeIn}},
    queryCount: 6,
    newQuery: {a: {$eq: 1}, b: {$in: evenLargerIn}},
    newQueryCount: 6,
    reuseEntry: true,
    isExplode: false
});
