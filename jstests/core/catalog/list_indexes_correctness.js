/**
 * Tests for the correctness of the response from the listIndexes command after DDL operations
 * that alter indexes or their options. This includes createIndex, dropIndex, collMod,
 * createCollection, shardCollection, dropCollection, and renameCollection operations
 * that modify index properties such as expireAfterSeconds, hidden, unique, prepareUnique,
 * sparse, etc.
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {IndexUtils} from "jstests/libs/index_utils.js";

const dbName = jsTestName();
const testDb = db.getSiblingDB(dbName);
const coll = testDb.getCollection("collTest");

// Detect if collections are implicitly sharded
const isImplicitlyShardedCollection = typeof globalThis.ImplicitlyShardAccessCollSettings !== "undefined";

function getIndexesFromListIndexes(collection) {
    const targetColl = collection || coll;
    return new DBCommandCursor(testDb, testDb.runCommand({listIndexes: targetColl.getName()})).toArray();
}

function getIndexesMustFail(errorCode, collection) {
    const targetColl = collection || coll;
    const res = testDb.runCommand({listIndexes: targetColl.getName()});
    assert.commandFailedWithCode(
        res,
        errorCode,
        `listIndexes should fail with ${errorCode} for ${collection.getFullName()}`,
    );
}

function findIndexByKey(indexes, keyPattern) {
    return indexes.find((idx) => bsonWoCompare(idx.key, keyPattern) === 0);
}

function findIndexByName(indexes, indexName) {
    return indexes.find((idx) => idx.name === indexName);
}

function dropCollection(coll) {
    if (isImplicitlyShardedCollection) {
        const originalImplicitlyShardOnCreateCollectionOnly = TestData.implicitlyShardOnCreateCollectionOnly;
        try {
            TestData.implicitlyShardOnCreateCollectionOnly = true;
            assert(coll.drop());
        } finally {
            TestData.implicitlyShardOnCreateCollectionOnly = originalImplicitlyShardOnCreateCollectionOnly;
        }
    } else {
        assert(coll.drop());
    }
}

describe("ListIndexesCorrectness", function () {
    beforeEach(() => {
        testDb.dropDatabase();
    });

    it("should list indexes correctly after createIndex", () => {
        // Create index {a:1}
        assert.commandWorked(coll.createIndex({a: 1}));

        // Verify index {a:1} exists
        {
            const indexes = getIndexesFromListIndexes();
            IndexUtils.assertIndexesMatch(
                coll,
                [{_id: 1}, {a: 1}],
                indexes.map((idx) => idx.key),
            );
        }
    });

    it("should list indexes correctly after dropIndex", () => {
        // Create index {a:1}
        assert.commandWorked(coll.createIndex({a: 1}));

        // Verify index {a:1} exists
        {
            const indexes = getIndexesFromListIndexes();
            IndexUtils.assertIndexesMatch(
                coll,
                [{_id: 1}, {a: 1}],
                indexes.map((idx) => idx.key),
            );
        }
        // Drop index {a:1}
        assert.commandWorked(coll.dropIndex({a: 1}));
        {
            const indexes = getIndexesFromListIndexes();
            IndexUtils.assertIndexesMatch(
                coll,
                [{_id: 1}],
                indexes.map((idx) => idx.key),
            );
        }
    });

    it("should list indexes correctly after collMod changes hidden property", () => {
        // Create index {b:1}
        assert.commandWorked(coll.createIndex({b: 1}));

        // Verify index {b:1} exists and is not hidden
        let indexB = findIndexByKey(getIndexesFromListIndexes(), {b: 1});
        assert.neq(undefined, indexB);
        assert.eq(undefined, indexB.hidden, "Index should not be hidden initially");

        // Use collMod to hide the index
        assert.commandWorked(
            testDb.runCommand({
                collMod: coll.getName(),
                index: {keyPattern: {b: 1}, hidden: true},
            }),
        );

        // Verify index {b:1} is now hidden
        indexB = findIndexByKey(getIndexesFromListIndexes(), {b: 1});
        assert.neq(undefined, indexB);
        assert.eq(true, indexB.hidden, "Index should be hidden after collMod");

        // Use collMod to unhide the index
        assert.commandWorked(
            testDb.runCommand({
                collMod: coll.getName(),
                index: {keyPattern: {b: 1}, hidden: false},
            }),
        );

        // Verify index {b:1} is no longer hidden
        indexB = findIndexByKey(getIndexesFromListIndexes(), {b: 1});
        assert.neq(undefined, indexB);
        assert.eq(undefined, indexB.hidden, "Index should not be hidden after unhiding");
    });

    it("should list indexes correctly after collMod changes expireAfterSeconds", () => {
        // Create a TTL index on {c:1} with expireAfterSeconds = 3600
        assert.commandWorked(coll.createIndex({c: 1}, {expireAfterSeconds: 3600}));

        // Verify the TTL index exists with correct expireAfterSeconds
        let indexC = findIndexByKey(getIndexesFromListIndexes(), {c: 1});
        assert.neq(undefined, indexC);
        assert.eq(3600, indexC.expireAfterSeconds, "Index should have expireAfterSeconds = 3600");

        // Use collMod to change expireAfterSeconds to 7200
        assert.commandWorked(
            testDb.runCommand({
                collMod: coll.getName(),
                index: {keyPattern: {c: 1}, expireAfterSeconds: 7200},
            }),
        );

        // Verify index {c:1} now has expireAfterSeconds = 7200
        indexC = findIndexByKey(getIndexesFromListIndexes(), {c: 1});
        assert.neq(undefined, indexC);
        assert.eq(7200, indexC.expireAfterSeconds, "Index should have expireAfterSeconds = 7200 after collMod");
    });

    // TODO (SERVER-118648): Enable this test once the bug is fixed.
    it.skip("should list indexes correctly after collMod converts an index to unique", () => {
        // Create a regular non-unique index on {e:1}
        assert.commandWorked(coll.createIndex({e: 1}));

        // Verify index {e:1} exists and is not unique or prepareUnique
        let indexE = findIndexByKey(getIndexesFromListIndexes(), {e: 1});
        assert.neq(undefined, indexE);
        assert.eq(undefined, indexE.unique, "Index should not be unique initially");
        assert.eq(undefined, indexE.prepareUnique, "Index should not have prepareUnique initially");

        // Use collMod to set prepareUnique to true
        assert.commandWorked(
            testDb.runCommand({
                collMod: coll.getName(),
                index: {keyPattern: {e: 1}, prepareUnique: true},
            }),
        );

        // Verify index {e:1} now has prepareUnique set to true
        indexE = findIndexByKey(getIndexesFromListIndexes(), {e: 1});
        assert.neq(undefined, indexE);
        assert.eq(true, indexE.prepareUnique, "Index should have prepareUnique = true");
        assert.eq(undefined, indexE.unique, "Index should not be unique yet");

        // Use collMod to convert prepareUnique to unique
        assert.commandWorked(
            testDb.runCommand({
                collMod: coll.getName(),
                index: {keyPattern: {e: 1}, unique: true},
            }),
        );

        // Verify index {e:1} is now unique and prepareUnique is removed
        indexE = findIndexByKey(getIndexesFromListIndexes(), {e: 1});
        assert.neq(undefined, indexE);
        assert.eq(true, indexE.unique, "Index should be unique after conversion");
        assert.eq(undefined, indexE.prepareUnique, "Index should no longer have prepareUnique property");
    });

    it("should list only _id index after createCollection", () => {
        assert.commandWorked(testDb.createCollection(coll.getName()));

        const indexes = getIndexesFromListIndexes();
        IndexUtils.assertIndexesMatch(
            coll,
            [{_id: 1}],
            indexes.map((idx) => idx.key),
        );
    });

    it("should return error for listIndexes on dropped collection", () => {
        // Populate the collection with an index
        assert.commandWorked(coll.createIndex({d: 1}));

        // Verify indexes exist
        let indexes = getIndexesFromListIndexes();
        assert.neq(undefined, findIndexByName(indexes, "_id_"), "_id_ index should exist");
        assert.neq(undefined, findIndexByKey(indexes, {d: 1}), "Index on {d: 1} should exist");

        // Drop the collection
        dropCollection(coll);

        // Verify listIndexes returns an error (NamespaceNotFound) for the dropped collection
        getIndexesMustFail(ErrorCodes.NamespaceNotFound, coll);
    });

    it("should return error for listIndexes on dropped database", () => {
        // Populate the collection with an index
        assert.commandWorked(coll.createIndex({e: 1}));

        // Verify indexes exist
        let indexes = getIndexesFromListIndexes();
        IndexUtils.assertIndexesMatch(
            coll,
            [{_id: 1}, {e: 1}],
            indexes.map((idx) => idx.key),
        );

        // Drop the database
        assert.commandWorked(testDb.dropDatabase());

        // Verify listIndexes returns an error (NamespaceNotFound) for the dropped collection
        getIndexesMustFail(ErrorCodes.NamespaceNotFound, coll);
    });

    it("should list indexes correctly after renameCollection", () => {
        if (TestData.runningWithShardStepdowns || TestData.runningWithStepdowns) {
            jsTest.log.info(
                "Skipping renameCollection test because running with shard stepdowns. Note that renameCollection is not idempotent and may fail if the collection has been renamed and the operation gets retried.",
            );
            return;
        }

        // Create indexes on the source collection with various properties
        assert.commandWorked(coll.createIndex({i: 1}));
        assert.commandWorked(coll.createIndex({j: 1}, {hidden: true}));
        assert.commandWorked(coll.createIndex({k: 1}, {unique: true}));

        // Verify indexes exist on source collection
        let indexesBefore = getIndexesFromListIndexes();
        IndexUtils.assertIndexesMatch(
            coll,
            [{_id: 1}, {i: 1}, {j: 1}, {k: 1}],
            indexesBefore.map((idx) => idx.key),
        );

        // Verify properties on indexes
        let indexJ = findIndexByKey(indexesBefore, {j: 1});
        assert.eq(true, indexJ.hidden, "Index on {j: 1} should be hidden");
        let indexK = findIndexByKey(indexesBefore, {k: 1});
        assert.eq(true, indexK.unique, "Index on {k: 1} should be unique");

        // Rename the collection to collTarget
        const targetName = "collTarget";
        const collTarget = testDb.getCollection(targetName);
        // Drop the target collection in case it's been implicitly createn on 'getCollection'
        dropCollection(collTarget);

        assert.commandWorked(
            testDb.adminCommand({
                renameCollection: coll.getFullName(),
                to: collTarget.getFullName(),
            }),
        );

        // Verify indexes exist on the renamed collection with same properties
        let indexesAfter = getIndexesFromListIndexes(collTarget);
        IndexUtils.assertIndexesMatch(
            collTarget,
            [{_id: 1}, {i: 1}, {j: 1}, {k: 1}],
            indexesAfter.map((idx) => idx.key),
        );

        // Verify properties are preserved after rename
        indexJ = findIndexByKey(indexesAfter, {j: 1});
        assert.eq(true, indexJ.hidden, "Index on {j: 1} should still be hidden after rename");
        indexK = findIndexByKey(indexesAfter, {k: 1});
        assert.eq(true, indexK.unique, "Index on {k: 1} should still be unique after rename");

        // Verify listIndexes fails on the old collection name
        getIndexesMustFail(ErrorCodes.NamespaceNotFound, coll);
    });

    it("should replace target indexes after renameCollection with dropTarget", () => {
        if (TestData.runningWithShardStepdowns || TestData.runningWithStepdowns) {
            jsTest.log.info(
                "Skipping renameCollection test because running with shard stepdowns. Note that renameCollection is not idempotent and may fail if the collection has been renamed and the operation gets retried.",
            );
            return;
        }

        const targetName = "collTarget";
        const targetColl = testDb.getCollection(targetName);
        if (
            FixtureHelpers.isSharded(targetColl) &&
            db.getSiblingDB("config").tags.findOne({ns: targetColl.getFullName()})
        ) {
            // There are suites that implicitly shard collections and then add tags to them.
            jsTest.log.info("Skipping rename because the target collection is sharded and has tags");
            return;
        }

        // Create indexes on the source collection
        assert.commandWorked(coll.createIndex({x: 1}));
        assert.commandWorked(coll.createIndex({y: 1}, {unique: true}));

        // Verify source collection indexes
        let sourceIndexes = getIndexesFromListIndexes();
        IndexUtils.assertIndexesMatch(
            coll,
            [{_id: 1}, {x: 1}, {y: 1}],
            sourceIndexes.map((idx) => idx.key),
        );
        let sourceIndexY = findIndexByKey(sourceIndexes, {y: 1});
        assert.eq(true, sourceIndexY.unique, "Source index on {y: 1} should be unique");

        // Create a target collection with different indexes
        assert.commandWorked(testDb.createCollection(targetName));
        assert.commandWorked(targetColl.createIndex({z: 1}));
        assert.commandWorked(targetColl.createIndex({w: 1}, {sparse: true}));

        // Verify target collection has its own indexes before rename
        let targetIndexesBefore = getIndexesFromListIndexes(targetColl);
        IndexUtils.assertIndexesMatch(
            targetColl,
            [{_id: 1}, {z: 1}, {w: 1}],
            targetIndexesBefore.map((idx) => idx.key),
        );
        let targetIndexW = findIndexByKey(targetIndexesBefore, {w: 1});
        assert.eq(true, targetIndexW.sparse, "Target index on {w: 1} should be sparse");

        // Rename source collection to target, dropping the existing target
        assert.commandWorked(
            testDb.adminCommand({
                renameCollection: coll.getFullName(),
                to: targetColl.getFullName(),
                dropTarget: true,
            }),
        );

        // Verify the renamed collection now has the source collection's indexes, not the target's
        let indexesAfter = getIndexesFromListIndexes(targetColl);
        IndexUtils.assertIndexesMatch(
            targetColl,
            [{_id: 1}, {x: 1}, {y: 1}],
            indexesAfter.map((idx) => idx.key),
        );

        // Verify the source's index properties are preserved
        let indexY = findIndexByKey(indexesAfter, {y: 1});
        assert.eq(true, indexY.unique, "Index on {y: 1} should still be unique after rename");

        // Verify the old target indexes (z and w) no longer exist
        assert.eq(undefined, findIndexByKey(indexesAfter, {z: 1}), "Target's old index on {z: 1} should not exist");
        assert.eq(undefined, findIndexByKey(indexesAfter, {w: 1}), "Target's old index on {w: 1} should not exist");

        // Verify listIndexes fails on the source collection name
        getIndexesMustFail(ErrorCodes.NamespaceNotFound, coll);
    });
});
