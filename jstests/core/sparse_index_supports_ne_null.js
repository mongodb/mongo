/**
 * Test that sparse indexes can be used for {$ne: null} queries. Includes tests for (sparse)
 * compound indexes and for cases when {$ne: null} is within an $elemMatch.
 *
 * Cannot run on a sharded collection because different shards may have different plans available
 * depending on how the collection is sharded. (For example, if one shard's index goes multikey,
 * but another's is still not multikey, they may need to use different plans for certain queries).
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */
import {getPlanStages, getWinningPlan} from "jstests/libs/analyze_plan.js";

const coll = db.sparse_index_supports_ne_null;
coll.drop();

function checkQuery({query, shouldUseIndex, nResultsExpected, indexKeyPattern}) {
    const explain = assert.commandWorked(coll.find(query).explain());
    const ixScans = getPlanStages(getWinningPlan(explain.queryPlanner), "IXSCAN");

    if (shouldUseIndex) {
        assert.gte(ixScans.length, 1, explain);
        assert.eq(ixScans[0].keyPattern, indexKeyPattern);
    } else {
        assert.eq(ixScans.length, 0, explain);
    }

    assert.eq(coll.find(query).itcount(), nResultsExpected);
}

// Non compound case.
(function() {
const query = {
    a: {$ne: null}
};
const elemMatchQuery = {
    a: {$elemMatch: {$ne: null}}
};
const keyPattern = {
    a: 1
};

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: {x: 1}}));
assert.commandWorked(coll.insert({a: null}));
assert.commandWorked(coll.insert({a: undefined}));

assert.commandWorked(coll.createIndex(keyPattern, {sparse: true}));

// Be sure the index is used.
checkQuery({query: query, shouldUseIndex: true, nResultsExpected: 2, indexKeyPattern: keyPattern});
checkQuery({
    query: elemMatchQuery,
    shouldUseIndex: true,
    nResultsExpected: 0,
    indexKeyPattern: keyPattern
});

// When the index becomes multikey, it cannot support {$ne: null} queries.
assert.commandWorked(coll.insert({a: [1, 2, 3]}));
checkQuery({query: query, shouldUseIndex: false, nResultsExpected: 3, indexKeyPattern: keyPattern});
// But it can support queries with {$ne: null} within an $elemMatch.
checkQuery({
    query: elemMatchQuery,
    shouldUseIndex: true,
    nResultsExpected: 1,
    indexKeyPattern: keyPattern
});
})();

// Compound case.
(function() {
const query = {
    a: {$ne: null}
};
const elemMatchQuery = {
    a: {$elemMatch: {$ne: null}}
};
const keyPattern = {
    a: 1,
    b: 1
};

coll.drop();
assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: {x: 1}, b: 1}));
assert.commandWorked(coll.insert({a: null, b: 1}));
assert.commandWorked(coll.insert({a: undefined, b: 1}));

assert.commandWorked(coll.createIndex(keyPattern, {sparse: true}));

// Be sure the index is used.
checkQuery({query: query, shouldUseIndex: true, nResultsExpected: 2, indexKeyPattern: keyPattern});
checkQuery({
    query: elemMatchQuery,
    shouldUseIndex: true,
    nResultsExpected: 0,
    indexKeyPattern: keyPattern
});

// When the index becomes multikey on the second field, it should still be usable.
assert.commandWorked(coll.insert({a: 1, b: [1, 2, 3]}));
checkQuery({query: query, shouldUseIndex: true, nResultsExpected: 3, indexKeyPattern: keyPattern});
checkQuery({
    query: elemMatchQuery,
    shouldUseIndex: true,
    nResultsExpected: 0,
    indexKeyPattern: keyPattern
});

// When the index becomes multikey on the first field, it should no longer be usable.
assert.commandWorked(coll.insert({a: [1, 2, 3], b: 1}));
checkQuery({query: query, shouldUseIndex: false, nResultsExpected: 4, indexKeyPattern: keyPattern});
// Queries which use a $elemMatch should still be able to use the index.
checkQuery({
    query: elemMatchQuery,
    shouldUseIndex: true,
    nResultsExpected: 1,
    indexKeyPattern: keyPattern
});
})();

// Nested field multikey with $elemMatch.
(function() {
const keyPattern = {
    "a.b.c.d": 1
};
coll.drop();
assert.commandWorked(coll.insert({a: {b: [{c: {d: 1}}]}}));
assert.commandWorked(coll.insert({a: {b: [{c: {d: {e: 1}}}]}}));
assert.commandWorked(coll.insert({a: {b: [{c: {d: null}}]}}));
assert.commandWorked(coll.insert({a: {b: [{c: {d: undefined}}]}}));

assert.commandWorked(coll.createIndex(keyPattern, {sparse: true}));

const query = {
    "a.b.c.d": {$ne: null}
};
// $elemMatch value can always use the index.
const elemMatchValueQuery = {
    "a.b.c.d": {$elemMatch: {$ne: null}}
};

// 'a.b' is multikey, so the index isn't used.
checkQuery({query: query, shouldUseIndex: false, nResultsExpected: 2, indexKeyPattern: keyPattern});
// Since the multikey portion is above the $elemMatch, the $elemMatch query may use the
// index.
checkQuery({
    query: elemMatchValueQuery,
    shouldUseIndex: true,
    nResultsExpected: 0,
    indexKeyPattern: keyPattern
});

// Make the index become multikey on 'a' (another field above the $elemMatch).
assert.commandWorked(coll.insert({a: [{b: [{c: {d: 1}}]}]}));
checkQuery({query: query, shouldUseIndex: false, nResultsExpected: 3, indexKeyPattern: keyPattern});
// The only multikey paths are still above the $elemMatch, queries which use a $elemMatch
// should still be able to use the index.
checkQuery({
    query: elemMatchValueQuery,
    shouldUseIndex: true,
    nResultsExpected: 0,
    indexKeyPattern: keyPattern
});

// Make the index multikey for 'a.b.c'. Now the $elemMatch query may not use the index.
assert.commandWorked(coll.insert({a: {b: [{c: [{d: 1}]}]}}));
checkQuery({query: query, shouldUseIndex: false, nResultsExpected: 4, indexKeyPattern: keyPattern});
checkQuery({
    query: elemMatchValueQuery,
    shouldUseIndex: true,
    nResultsExpected: 0,
    indexKeyPattern: keyPattern
});
})();