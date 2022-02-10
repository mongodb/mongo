/**
 * Test that cache entries are removed and plan cache size is decreased on Plan Cache invalidation
 * and clearing.
 */

(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB("sbe_plan_cache_invalidation");

function getPlanCacheSize() {
    return db.runCommand({serverStatus: 1}).metrics.query.planCacheTotalSizeEstimateBytes;
}

/**
 * Helper class that creates a collection, indexes on it, and makes a few queries to add entries to
 * the plan cache.
 */
class TestCollection {
    constructor(name = "coll") {
        this.coll = db[name];
        this.coll.drop();

        this.indexNames = ["index1", "index2", "index3"];
        assert.commandWorked(this.coll.createIndex({a: 1}, {name: this.indexNames[0]}));
        assert.commandWorked(this.coll.createIndex({b: 1}, {name: this.indexNames[1]}));
        assert.commandWorked(this.coll.createIndex({b: 1, a: 1}, {name: this.indexNames[2]}));

        assert.eq(0, this.coll.find({a: 1, b: 2}).itcount());
        assert.eq(0, this.coll.find({a: 1, c: 3, b: 2}).itcount());

        assert.gt(getPlanCacheSize(), 0);

        this.nCacheEntries = this.getNumberOfPlanCacheEntries();
        assert.eq(2, this.nCacheEntries);
    }

    getNumberOfPlanCacheEntries() {
        return this.coll.getPlanCache().list().length;
    }

    assertAllCollectionCacheEntriesRemoved() {
        assert.eq(0, this.getNumberOfPlanCacheEntries());
    }

    assertCacheEntriesNotRemoved() {
        assert.eq(this.nCacheEntries, this.getNumberOfPlanCacheEntries());
    }
}

(function cacheEntriesRemovedIfTheCollectionDropped() {
    const initialPlanCacheSize = getPlanCacheSize();
    const test = new TestCollection();

    assert.gt(getPlanCacheSize(), initialPlanCacheSize);

    assert(test.coll.drop());

    assert.eq(getPlanCacheSize(), initialPlanCacheSize);
}());

(function cacheEntriesNotRemovedIfAnotherCollectedDropped() {
    const test = new TestCollection("coll1");
    const cacheSizeForOneTestCollection = getPlanCacheSize();

    const anotherTest = new TestCollection("coll2");
    const cacheSizeForTwoTestCollections = getPlanCacheSize();
    assert.gt(cacheSizeForTwoTestCollections, cacheSizeForOneTestCollection);
    assert(anotherTest.coll.drop());

    assert.eq(cacheSizeForOneTestCollection, getPlanCacheSize());
    test.assertCacheEntriesNotRemoved();
}());

(function cacheEntriesRemovedIfANewIndexCreated() {
    const test = new TestCollection();
    const planCacheSize = getPlanCacheSize();

    assert.commandWorked(test.coll.createIndex({hi: 1}));

    assert.lt(getPlanCacheSize(), planCacheSize);
    test.assertAllCollectionCacheEntriesRemoved();
}());

(function cacheEntriesRemovedIfAnyIndexDropped() {
    const test = new TestCollection();
    const initialPlanCacheSize = getPlanCacheSize();

    assert.commandWorked(test.coll.dropIndex(test.indexNames[0]));

    assert.lt(getPlanCacheSize(), initialPlanCacheSize);
    test.assertAllCollectionCacheEntriesRemoved();
}());

(function cacheEntriesNotRemovedIfCallModCalled() {
    const collectionName = "coll";
    const test = new TestCollection(collectionName);
    const initialPlanCacheSize = getPlanCacheSize();

    assert.commandWorked(
        db.runCommand({collMod: collectionName, validator: {text: {$type: "string"}}}));

    assert.eq(getPlanCacheSize(), initialPlanCacheSize);
    test.assertCacheEntriesNotRemoved();
}());

(function cacheEntriesRemovedIfIndexChanged() {
    const collectionName = "coll";
    const test = new TestCollection(collectionName);
    const initialPlanCacheSize = getPlanCacheSize();

    assert.commandWorked(db.runCommand({
        collMod: collectionName,
        index: {
            name: test.indexNames[0],
            hidden: true,
        }
    }));

    assert.lt(getPlanCacheSize(), initialPlanCacheSize);
    test.assertAllCollectionCacheEntriesRemoved();
}());

(function cacheEntriesRemovedOnClearPlanCacheCommand() {
    const collectionName = "coll";
    const initialPlanCacheSize = getPlanCacheSize();

    const test = new TestCollection(collectionName);
    assert.gt(getPlanCacheSize(), initialPlanCacheSize);

    assert.commandWorked(db.runCommand({planCacheClear: collectionName}));

    assert.eq(getPlanCacheSize(), initialPlanCacheSize);
    test.assertAllCollectionCacheEntriesRemoved();
}());

(function oneCacheEntryRemovedOnClearPlanCacheWithQueryCommand() {
    const collectionName = "coll";
    const test = new TestCollection(collectionName);
    const numberOfCacheEntries = test.getNumberOfPlanCacheEntries();

    test.coll.find({a: 1, b: 2, c: 3, d: 4}).itcount();
    assert.eq(numberOfCacheEntries + 1, test.getNumberOfPlanCacheEntries());
    const planCacheSize = getPlanCacheSize();

    assert.commandWorked(
        db.runCommand({planCacheClear: collectionName, query: {a: 1, b: 2, c: 3, d: 4}}));
    assert.lt(getPlanCacheSize(), planCacheSize);
    assert.eq(numberOfCacheEntries, test.getNumberOfPlanCacheEntries());
}());

MongoRunner.stopMongod(conn);
}());
