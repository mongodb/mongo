/**
 * Verifies that $or queries on clustered collections that have plans with IXSCAN and
 * CLUSTERED_IXSCAN stages does not use the SBE plan cache.
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

const mongod = MongoRunner.runMongod();
const dbName = "test";
const db = mongod.getDB(dbName);
const coll = db.or_use_clustered_collection;
assertDropCollection(db, coll.getName());

// Create a clustered collection and create indexes.
assert.commandWorked(
    db.createCollection(coll.getName(), {clusteredIndex: {key: {_id: 1}, unique: true}}));
assert.commandWorked(coll.createIndex({a: 1}));

// Insert documents, and store them to be used later in the test.
const docs = [];
const numDocs = 10;
for (let i = 0; i < numDocs; i++) {
    docs.push({a: i, _id: i, noIndex: i});
}
assert.commandWorked(coll.insertMany(docs));

function assertCorrectResults({query, expectedDocIds}) {
    let results = query.toArray();
    let expectedResults = [];
    expectedDocIds.forEach(id => expectedResults.push(docs[id]));
    assert.sameMembers(results, expectedResults);
}

function validatePlanCacheEntries({increment, query, expectedDocIds}) {
    const oldSize = coll.getPlanCache().list().length;
    assertCorrectResults({query: query, expectedDocIds: expectedDocIds});
    const newSize = coll.getPlanCache().list().length;
    assert.eq(oldSize + increment,
              newSize,
              "Expected " + tojson(increment) +
                  " new entries in the cache, but got: " + tojson(coll.getPlanCache().list()));
}

coll.getPlanCache().clear();
// Validate queries with a single equality clustered collection scan.
validatePlanCacheEntries(
    {increment: 0, query: coll.find({$or: [{_id: 123}, {a: 12}]}), expectedDocIds: []});
validatePlanCacheEntries(
    {increment: 0, query: coll.find({$or: [{_id: 6}, {a: 5}]}), expectedDocIds: [5, 6]});

// Validate queries with multiple equality clustered collection scans.
validatePlanCacheEntries(
    {increment: 0, query: coll.find({$or: [{_id: 100}, {_id: 123}, {a: 11}]}), expectedDocIds: []});
validatePlanCacheEntries({
    increment: 0,
    query: coll.find({$or: [{_id: 9}, {_id: 5}, {a: 4}]}),
    expectedDocIds: [4, 5, 9]
});

// Validate queries with multiple range clustered collection scans.
validatePlanCacheEntries({
    increment: 0,
    query: coll.find({$or: [{_id: {$lt: -1}}, {_id: {$gt: 10}}, {a: 12}]}),
    expectedDocIds: []
});
validatePlanCacheEntries({
    increment: 0,
    query: coll.find({$or: [{_id: {$lt: 1}}, {_id: {$gt: 8}}, {a: 4}]}),
    expectedDocIds: [0, 4, 9]
});

// Validate queries with both range and equality clustered collection scans.
validatePlanCacheEntries({
    increment: 0,
    query: coll.find({$or: [{_id: {$lt: -1}}, {_id: 11}, {a: 12}]}),
    expectedDocIds: []
});
validatePlanCacheEntries({
    increment: 0,
    query: coll.find({$or: [{_id: {$lt: 2}}, {_id: 8}, {a: 4}]}),
    expectedDocIds: [0, 1, 4, 8]
});

// Validate queries with 'max' and 'min' set have the correct results. These plans fall back to
// collection scans by the query planner for clustered collections.
validatePlanCacheEntries({
    increment: 0,
    query: coll.find({$or: [{_id: 123}, {a: 12}]}).max({_id: 4}).hint({_id: 1}),
    expectedDocIds: []
});
validatePlanCacheEntries({
    increment: 0,
    query: coll.find({$or: [{_id: 6}, {a: 5}]}).max({_id: 6}).hint({_id: 1}),
    expectedDocIds: [5]
});

validatePlanCacheEntries({
    increment: 0,
    query: coll.find({$or: [{_id: 8}, {a: 5}]}).min({_id: 6}).hint({_id: 1}),
    expectedDocIds: [8]
});
validatePlanCacheEntries({
    increment: 0,
    query: coll.find({$or: [{_id: 123}, {a: 12}]}).min({_id: 4}).hint({_id: 1}),
    expectedDocIds: []
});

// Validate queries that just use a collection scan still get cached. We are checking the SBE cache,
// and don't expect it to increment for classic.
const incrementCache = checkSbeFullFeatureFlagEnabled(db) ? 1 : 0;
validatePlanCacheEntries({
    increment: incrementCache,
    query: coll.find({_id: {$gte: 4}}),
    expectedDocIds: [4, 5, 6, 7, 8, 9]
});

validatePlanCacheEntries({
    increment: incrementCache,
    query: coll.find({$and: [{_id: {$gte: 4}}, {noIndex: 6}]}),
    expectedDocIds: [6]
});

MongoRunner.stopMongod(mongod);
