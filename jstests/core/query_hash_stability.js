/**
 * Test that 'queryHash' and 'planCacheKey' from explain() output have sensible values
 * across catalog changes.
 */
(function() {
"use strict";
load('jstests/libs/fixture_helpers.js');  // For and isMongos().

const collName = "query_hash_stability";
const coll = db[collName];
coll.drop();
// Be sure the collection exists.
assert.commandWorked(coll.insert({x: 5}));

function getPlanCacheKeyFromExplain(explainRes) {
    const hash = FixtureHelpers.isMongos(db)
        ? explainRes.queryPlanner.winningPlan.shards[0].planCacheKey
        : explainRes.queryPlanner.planCacheKey;
    assert.eq(typeof (hash), "string");
    return hash;
}

function getQueryHashFromExplain(explainRes) {
    const hash = FixtureHelpers.isMongos(db)
        ? explainRes.queryPlanner.winningPlan.shards[0].queryHash
        : explainRes.queryPlanner.queryHash;
    assert.eq(typeof (hash), "string");
    return hash;
}

const query = {
    x: 3
};

const initialExplain = coll.find(query).explain();

// Add a sparse index.
assert.commandWorked(coll.createIndex({x: 1}, {sparse: true}));

const withIndexExplain = coll.find(query).explain();

// 'queryHash' shouldn't change across catalog changes.
assert.eq(getQueryHashFromExplain(initialExplain), getQueryHashFromExplain(withIndexExplain));
// We added an index so the plan cache key changed.
assert.neq(getPlanCacheKeyFromExplain(initialExplain),
           getPlanCacheKeyFromExplain(withIndexExplain));

// Drop the index.
assert.commandWorked(coll.dropIndex({x: 1}));
const postDropExplain = coll.find(query).explain();

// 'queryHash' shouldn't change across catalog changes.
assert.eq(getQueryHashFromExplain(initialExplain), getQueryHashFromExplain(postDropExplain));

// The 'planCacheKey' should be the same as what it was before we dropped the index.
assert.eq(getPlanCacheKeyFromExplain(initialExplain), getPlanCacheKeyFromExplain(postDropExplain));
})();
