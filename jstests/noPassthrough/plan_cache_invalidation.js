/**
 * Test that cache entries are removed and plan cache size is decreased on Plan Cache invalidation
 * and clearing.
 * @tags: [
 * ]
 */

import {
    checkExperimentalCascadesOptimizerEnabled,
    runWithParamsAllNodes
} from "jstests/libs/optimizer_utils.js";
import {getPlanCacheNumEntries, getPlanCacheSize} from "jstests/libs/plan_cache_utils.js";

const conn = MongoRunner.runMongod();
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB("sbe_plan_cache_invalidation");

// TODO SERVER-85728: Enable Bonsai plan cache tests involving indices. We run a modified version of
// this test without indexes to obtain coverage of plan cache invalidation logic.
const isBonsaiExperimental = checkExperimentalCascadesOptimizerEnabled(db);

// Disable CQF fast path for queries testing the plan cache. Fast path queries do not
// get cached and may break tests that expect cache entries for fast path-eligible queries.
runWithParamsAllNodes(db, [{key: "internalCascadesOptimizerDisableFastPath", value: true}], () => {
    /**
     * Helper class that creates a collection, indexes on it, and makes a few queries to add entries
     * to the plan cache.
     */
    class TestCollection {
        constructor(name = "coll") {
            this.coll = db[name];
            this.coll.drop();

            this.indexNames = ["index1", "index2", "index3"];
            if (isBonsaiExperimental) {
                assert.commandWorked(this.coll.insert({unused_field: 1}));
            } else {
                assert.commandWorked(this.coll.createIndex({a: 1}, {name: this.indexNames[0]}));
                assert.commandWorked(this.coll.createIndex({b: 1}, {name: this.indexNames[1]}));
                assert.commandWorked(
                    this.coll.createIndex({b: 1, a: 1}, {name: this.indexNames[2]}));
            }

            assert.eq(0, this.coll.find({a: 1, b: 2}).itcount());
            assert.eq(0, this.coll.find({a: 1, c: 3, b: 2}).itcount());

            assert.gt(getPlanCacheSize(db), 0);

            this.nCacheEntries = this.getNumberOfCollectionPlanCacheEntries();
            assert.eq(2, this.nCacheEntries);
        }
        // The following three helper functions concern plan cache entries specific to a given
        // collection, and not the entire/global plan cache
        getNumberOfCollectionPlanCacheEntries() {
            return this.coll.getPlanCache().list().length;
        }

        assertAllCollectionCacheEntriesRemoved() {
            assert.eq(0, this.getNumberOfCollectionPlanCacheEntries());
        }

        assertCollectionCacheEntriesNotRemoved() {
            assert.eq(this.nCacheEntries, this.getNumberOfCollectionPlanCacheEntries());
        }
    }

    (function cacheEntriesRemovedIfTheCollectionDropped() {
        const initialPlanCacheSize = getPlanCacheSize(db);
        const test = new TestCollection();

        assert.gt(getPlanCacheSize(db), initialPlanCacheSize);
        assert.eq(getPlanCacheNumEntries(db), 2);

        assert(test.coll.drop());
        assert.eq(getPlanCacheSize(db), initialPlanCacheSize);
        assert.eq(getPlanCacheNumEntries(db), 0);
    }());

    (function cacheEntriesNotRemovedIfAnotherCollectedDropped() {
        const test = new TestCollection("coll1");
        const cacheSizeForOneTestCollection = getPlanCacheSize(db);
        assert.eq(getPlanCacheNumEntries(db), 2);

        const anotherTest = new TestCollection("coll2");
        const cacheSizeForTwoTestCollections = getPlanCacheSize(db);
        assert.eq(getPlanCacheNumEntries(db), 4);
        assert.gt(cacheSizeForTwoTestCollections, cacheSizeForOneTestCollection);
        assert(anotherTest.coll.drop());

        assert.eq(cacheSizeForOneTestCollection, getPlanCacheSize(db));
        // Entries associated with anotherTest.coll are booted from the plan cache.
        assert.eq(getPlanCacheNumEntries(db), 2);
        test.assertCollectionCacheEntriesNotRemoved();
    }());

    (function cacheEntriesRemovedIfANewIndexCreated() {
        const test = new TestCollection();
        const planCacheSize = getPlanCacheSize(db);

        assert.commandWorked(test.coll.createIndex({hi: 1}));

        assert.lt(getPlanCacheSize(db), planCacheSize);
        test.assertAllCollectionCacheEntriesRemoved();
    }());

    (function cacheEntriesRemovedIfAnyIndexDropped() {
        if (isBonsaiExperimental) {
            return;
        }
        const test = new TestCollection();
        const initialPlanCacheSize = getPlanCacheSize(db);

        assert.commandWorked(test.coll.dropIndex(test.indexNames[0]));

        assert.lt(getPlanCacheSize(db), initialPlanCacheSize);
        test.assertAllCollectionCacheEntriesRemoved();
    }());

    (function cacheEntriesNotRemovedIfCollModCalled() {
        const collectionName = "coll";
        const test = new TestCollection(collectionName);
        const initialPlanCacheSize = getPlanCacheSize(db);

        assert.commandWorked(
            db.runCommand({collMod: collectionName, validator: {text: {$type: "string"}}}));

        assert.eq(getPlanCacheSize(db), initialPlanCacheSize);
        test.assertCollectionCacheEntriesNotRemoved();
    }());

    (function cacheEntriesRemovedIfIndexChanged() {
        if (isBonsaiExperimental) {
            return;
        }
        const collectionName = "coll";
        const test = new TestCollection(collectionName);
        const initialPlanCacheSize = getPlanCacheSize(db);
        assert.commandWorked(db.runCommand({
            collMod: collectionName,
            index: {
                name: test.indexNames[0],
                hidden: true,
            }
        }));
        assert.lt(getPlanCacheSize(db), initialPlanCacheSize);
        test.assertAllCollectionCacheEntriesRemoved();
    }());

    (function cacheEntriesRemovedOnClearPlanCacheCommand() {
        const collectionName = "cacheEntriesRemovedOnClearPlanCacheCommand";
        // We get the plan cache size here which includes state from previous tests. Use a unique
        // collection name to ensure the plan cache size will strictly increase. If we reused the
        // collection name from previous tests, entries for the previous test would be cleared and
        // we can't make any guarantees.
        const initialPlanCacheSize = getPlanCacheSize(db);

        const test = new TestCollection(collectionName);
        assert.gt(getPlanCacheSize(db), initialPlanCacheSize);

        assert.commandWorked(db.runCommand({planCacheClear: collectionName}));

        assert.eq(getPlanCacheSize(db), initialPlanCacheSize);
        test.assertAllCollectionCacheEntriesRemoved();
    }());

    (function oneCacheEntryRemovedOnClearPlanCacheWithQueryCommand() {
        const collectionName = "coll";
        const test = new TestCollection(collectionName);
        const numberOfCacheEntries = getPlanCacheNumEntries(db);

        test.coll.find({a: 1, b: 2, c: 3, d: 4}).itcount();
        assert.eq(numberOfCacheEntries + 1, getPlanCacheNumEntries(db));
        const planCacheSize = getPlanCacheSize(db);

        assert.commandWorked(
            db.runCommand({planCacheClear: collectionName, query: {a: 1, b: 2, c: 3, d: 4}}));
        assert.lt(getPlanCacheSize(db), planCacheSize);
        assert.eq(numberOfCacheEntries, getPlanCacheNumEntries(db));
    }());
});

MongoRunner.stopMongod(conn);
