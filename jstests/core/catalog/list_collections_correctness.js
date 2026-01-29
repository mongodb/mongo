/**
 * Tests for the correctness of the response from the listCollections command after DDL operations
 * that alter collections or their options. This includes createCollection, dropCollection,
 * renameCollection, collMod, dropDatabase, convertToCapped, and shardCollection operations
 * that modify collection properties such as validator, validationLevel, validationAction,
 * capped, size, max, etc.
 */

import {beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const dbName = jsTestName();
const testDb = db.getSiblingDB(dbName);
const collName = "collTest";
const coll = testDb.getCollection(collName);

// Detect if collections are implicitly sharded
const isImplicitlyShardedCollection = typeof globalThis.ImplicitlyShardAccessCollSettings !== "undefined";

function getCollectionsFromListCollections(dbObj, filter = {}) {
    const db = dbObj || testDb;
    return new DBCommandCursor(db, db.runCommand({listCollections: 1, filter: filter})).toArray();
}

function findCollectionByName(collections, name) {
    return collections.find((c) => c.name === name);
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

describe("ListCollectionsCorrectness", function () {
    beforeEach(() => {
        testDb.dropDatabase();
    });

    it("should list collection correctly after createCollection", () => {
        // Create collection
        assert.commandWorked(testDb.createCollection(collName));

        // Verify collection exists
        const collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(
            1,
            collections.length,
            `Collection should exist after creation. Found ${collections.length} collections: ${tojson(collections)}`,
        );
        const collInfo = collections[0];
        assert.eq(
            collName,
            collInfo.name,
            `Collection name should match. Expected: ${collName}, Actual: ${collInfo.name}`,
        );
        assert.eq("collection", collInfo.type, `Type should be 'collection'. Actual: ${collInfo.type}`);
    });

    it("should list collection with options after createCollection with options", () => {
        // Create collection with validator
        const validator = {$jsonSchema: {required: ["name"], properties: {name: {bsonType: "string"}}}};
        assert.commandWorked(
            testDb.createCollection(collName, {
                validator: validator,
                validationLevel: "moderate",
                validationAction: "warn",
            }),
        );

        // Verify collection exists with correct options
        const collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(1, collections.length, `Collection should exist. Found ${collections.length} collections`);
        const collInfo = collections[0];
        assert.eq(
            collName,
            collInfo.name,
            `Collection name should match. Expected: ${collName}, Actual: ${collInfo.name}`,
        );
        assert.docEq(
            validator,
            collInfo.options.validator,
            `Validator should match. Expected: ${tojson(validator)}, Actual: ${tojson(collInfo.options.validator)}`,
        );
        assert.eq(
            "moderate",
            collInfo.options.validationLevel,
            `Validation level should match. Expected: 'moderate', Actual: ${collInfo.options.validationLevel}`,
        );
        assert.eq(
            "warn",
            collInfo.options.validationAction,
            `Validation action should match. Expected: 'warn', Actual: ${collInfo.options.validationAction}`,
        );
    });

    it("should list capped collection correctly after createCollection with capped options", () => {
        if (isImplicitlyShardedCollection) {
            jsTest.log.info(
                "Skipping createCollection with capped options test because collections are implicitly sharded and sharded collections can't be capped.",
            );
            return;
        }

        // Create capped collection
        const cappedSize = 1024 * 1024; // 1MB
        const cappedMax = 100;
        assert.commandWorked(
            testDb.createCollection(collName, {
                capped: true,
                size: cappedSize,
                max: cappedMax,
            }),
        );

        // Verify collection exists with capped options
        const collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(1, collections.length, `Collection should exist. Found ${collections.length} collections`);
        const collInfo = collections[0];
        assert.eq(
            collName,
            collInfo.name,
            `Collection name should match. Expected: ${collName}, Actual: ${collInfo.name}`,
        );
        assert.eq(true, collInfo.options.capped, `Collection should be capped. Actual: ${collInfo.options.capped}`);
        assert.eq(
            cappedSize,
            collInfo.options.size,
            `Capped size should match. Expected: ${cappedSize}, Actual: ${collInfo.options.size}`,
        );
        assert.eq(
            cappedMax,
            collInfo.options.max,
            `Capped max should match. Expected: ${cappedMax}, Actual: ${collInfo.options.max}`,
        );
    });

    it("should not list collection after dropCollection", () => {
        // Create collection
        assert.commandWorked(testDb.createCollection(collName));

        // Verify collection exists
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(
            1,
            collections.length,
            `Collection should exist before drop. Found ${collections.length} collections`,
        );

        // Drop collection
        dropCollection(coll);

        // Verify collection no longer exists
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(
            0,
            collections.length,
            `Collection should not exist after drop. Found ${collections.length} collections: ${tojson(collections)}`,
        );
    });

    it("should update collection options after collMod changes validator", () => {
        // Create collection with initial validator
        const initialValidator = {$jsonSchema: {required: ["x"], properties: {x: {bsonType: "int"}}}};
        assert.commandWorked(testDb.createCollection(collName, {validator: initialValidator}));

        // Verify initial validator
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        let collInfo = findCollectionByName(collections, collName);
        assert.neq(
            undefined,
            collInfo,
            `Collection '${collName}' should exist. Collections: ${tojson(collections.map((c) => c.name))}`,
        );
        assert.docEq(
            initialValidator,
            collInfo.options.validator,
            `Initial validator should match. Expected: ${tojson(initialValidator)}, Actual: ${tojson(collInfo.options.validator)}`,
        );

        // Use collMod to change validator
        const newValidator = {$jsonSchema: {required: ["y"], properties: {y: {bsonType: "string"}}}};
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validator: newValidator,
            }),
        );

        // Verify validator has been updated
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        collInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, collInfo, `Collection '${collName}' should exist after collMod`);
        assert.docEq(
            newValidator,
            collInfo.options.validator,
            `Validator should be updated after collMod. Expected: ${tojson(newValidator)}, Actual: ${tojson(collInfo.options.validator)}`,
        );
    });

    it("should update collection options after collMod changes validationLevel", () => {
        // Create collection with validator and initial validation level
        const validator = {$jsonSchema: {required: ["a"], properties: {a: {bsonType: "int"}}}};
        assert.commandWorked(
            testDb.createCollection(collName, {
                validator: validator,
                validationLevel: "strict",
            }),
        );

        // Verify initial validation level
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        let collInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, collInfo, `Collection '${collName}' should exist`);
        assert.eq(
            "strict",
            collInfo.options.validationLevel,
            `Initial validation level should be strict. Actual: ${collInfo.options.validationLevel}`,
        );

        // Use collMod to change validation level
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validationLevel: "moderate",
            }),
        );

        // Verify validation level has been updated
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        collInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, collInfo, `Collection '${collName}' should exist after collMod`);
        assert.eq(
            "moderate",
            collInfo.options.validationLevel,
            `Validation level should be moderate after collMod. Actual: ${collInfo.options.validationLevel}`,
        );
    });

    it("should update collection options after collMod changes validationAction", () => {
        // Create collection with validator and initial validation action
        const validator = {$jsonSchema: {required: ["b"], properties: {b: {bsonType: "string"}}}};
        assert.commandWorked(
            testDb.createCollection(collName, {
                validator: validator,
                validationAction: "error",
            }),
        );

        // Verify initial validation action
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        let collInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, collInfo, `Collection '${collName}' should exist`);
        assert.eq(
            "error",
            collInfo.options.validationAction,
            `Initial validation action should be error. Actual: ${collInfo.options.validationAction}`,
        );

        // Use collMod to change validation action
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validationAction: "warn",
            }),
        );

        // Verify validation action has been updated
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        collInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, collInfo, `Collection '${collName}' should exist after collMod`);
        assert.eq(
            "warn",
            collInfo.options.validationAction,
            `Validation action should be warn after collMod. Actual: ${collInfo.options.validationAction}`,
        );
    });

    it("should list collection with new name after renameCollection", () => {
        if (TestData.runningWithShardStepdowns || TestData.runningWithStepdowns) {
            jsTest.log.info(
                "Skipping renameCollection test because running with shard stepdowns. Note that renameCollection is not idempotent and may fail if the collection has been renamed and the operation gets retried.",
            );
            return;
        }

        // Create collection
        assert.commandWorked(testDb.createCollection(coll.getName()));

        // Add validator for testing that options are preserved
        const validator = {$jsonSchema: {required: ["field"], properties: {field: {bsonType: "int"}}}};
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validator: validator,
                validationLevel: "strict",
            }),
        );

        // Verify collection exists with options
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        let collInfo = findCollectionByName(collections, collName);
        assert.neq(
            undefined,
            collInfo,
            `Source collection should exist. Collections: ${tojson(collections.map((c) => c.name))}`,
        );
        assert.docEq(
            validator,
            collInfo.options.validator,
            `Validator should be set. Expected: ${tojson(validator)}, Actual: ${tojson(collInfo.options.validator)}`,
        );
        assert.eq(
            "strict",
            collInfo.options.validationLevel,
            `Validation level should be strict. Actual: ${collInfo.options.validationLevel}`,
        );

        // Rename collection
        const targetName = "collRenamed";
        assert.commandWorked(
            testDb.adminCommand({
                renameCollection: coll.getFullName(),
                to: testDb.getName() + "." + targetName,
            }),
        );

        // Verify old collection name doesn't exist
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(
            0,
            collections.length,
            `Old collection name should not exist after rename. Found ${collections.length} collections: ${tojson(collections)}`,
        );

        // Verify new collection name exists with same options
        collections = getCollectionsFromListCollections(testDb, {name: targetName});
        collInfo = findCollectionByName(collections, targetName);
        assert.neq(
            undefined,
            collInfo,
            `Renamed collection should exist. Collections: ${tojson(collections.map((c) => c.name))}`,
        );
        assert.docEq(
            validator,
            collInfo.options.validator,
            `Validator should be preserved after rename. Expected: ${tojson(validator)}, Actual: ${tojson(collInfo.options.validator)}`,
        );
        assert.eq(
            "strict",
            collInfo.options.validationLevel,
            `Validation level should be preserved after rename. Actual: ${collInfo.options.validationLevel}`,
        );
    });

    it("should replace target collection after renameCollection with dropTarget", () => {
        if (TestData.runningWithShardStepdowns) {
            jsTest.log.info(
                "Skipping renameCollection test because running with shard stepdowns. Note that renameCollection is not idempotent and may fail if the collection has been renamed and the operation gets retried.",
            );
        }

        // Create source collection with validator
        assert.commandWorked(testDb.createCollection(collName));
        const sourceValidator = {$jsonSchema: {required: ["source"], properties: {source: {bsonType: "string"}}}};
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validator: sourceValidator,
            }),
        );

        // Verify source collection has its validator
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        let sourceInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, sourceInfo, `Source collection should exist`);
        assert.docEq(
            sourceValidator,
            sourceInfo.options.validator,
            `Source validator should be set. Expected: ${tojson(sourceValidator)}, Actual: ${tojson(sourceInfo.options.validator)}`,
        );

        // Create target collection with different validator
        const targetName = "collTarget";
        const targetColl = testDb.getCollection(targetName);
        assert.commandWorked(testDb.createCollection(targetName));
        const targetValidator = {$jsonSchema: {required: ["target"], properties: {target: {bsonType: "int"}}}};
        assert.commandWorked(
            testDb.runCommand({
                collMod: targetName,
                validator: targetValidator,
            }),
        );

        // Verify target collection has its own validator
        collections = getCollectionsFromListCollections(testDb, {name: targetName});
        let targetInfo = findCollectionByName(collections, targetName);
        assert.neq(undefined, targetInfo, `Target collection should exist`);
        assert.docEq(
            targetValidator,
            targetInfo.options.validator,
            `Target validator should be set. Expected: ${tojson(targetValidator)}, Actual: ${tojson(targetInfo.options.validator)}`,
        );

        if (
            FixtureHelpers.isSharded(targetColl) &&
            db.getSiblingDB("config").tags.findOne({ns: targetColl.getFullName()})
        ) {
            // There are suites that implicitly shard collections and then add tags to them.
            jsTest.log.info("Skipping rename because the target collection is sharded and has tags");
            return;
        }

        if (TestData.runningWithShardStepdowns || TestData.runningWithStepdowns) {
            jsTest.log.info(
                "Skipping renameCollection test because running with shard stepdowns. Note that renameCollection is not idempotent and may fail if the collection has been renamed and the operation gets retried.",
            );
            return;
        }

        // Rename source to target with dropTarget
        assert.commandWorked(
            testDb.adminCommand({
                renameCollection: coll.getFullName(),
                to: targetColl.getFullName(),
                dropTarget: true,
            }),
        );

        // Verify target collection now has source's validator, not its original validator
        collections = getCollectionsFromListCollections(testDb, {name: targetName});
        const finalInfo = findCollectionByName(collections, targetName);
        assert.neq(undefined, finalInfo, `Renamed collection should exist at target name`);
        assert.docEq(
            sourceValidator,
            finalInfo.options.validator,
            `Target should now have source's validator. Expected: ${tojson(sourceValidator)}, Actual: ${tojson(finalInfo.options.validator)}`,
        );

        // Verify old collection name doesn't exist
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(
            0,
            collections.length,
            `Old collection name should not exist after rename. Found ${collections.length} collections: ${tojson(collections)}`,
        );
    });

    it("should not list any collections after dropDatabase", () => {
        // Create multiple collections
        assert.commandWorked(testDb.createCollection(collName));
        assert.commandWorked(testDb.createCollection("coll2"));
        assert.commandWorked(testDb.createCollection("coll3"));

        // Verify collections exist
        let collections = getCollectionsFromListCollections(testDb);
        assert.gte(
            collections.length,
            3,
            `Should have at least 3 collections. Found ${collections.length} collections: ${tojson(collections.map((c) => c.name))}`,
        );

        // Drop database
        assert.commandWorked(testDb.dropDatabase());

        // Verify no user collections exist (may still have system collections)
        collections = getCollectionsFromListCollections(testDb);
        assert.eq(
            0,
            collections.length,
            `Should have no user collections after dropDatabase. Found ${collections.length} collections: ${tojson(collections.map((c) => c.name))}`,
        );
    });

    it("should show capped collection after convertToCapped", () => {
        if (isImplicitlyShardedCollection) {
            jsTest.log.info(
                "Skipping convertToCapped test because collections are implicitly sharded and sharded collections can't be capped.",
            );
            return;
        }

        // Create regular (non-capped) collection
        assert.commandWorked(testDb.createCollection(collName));

        // Verify collection is not capped
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        let collInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, collInfo, `Collection '${collName}' should exist`);
        assert.eq(
            undefined,
            collInfo.options.capped,
            `Collection should not be capped initially. Actual: ${collInfo.options.capped}`,
        );

        // Convert to capped
        const cappedSize = 10240; // 10KB
        assert.commandWorked(
            testDb.runCommand({
                convertToCapped: collName,
                size: cappedSize,
            }),
        );

        // Verify collection is now capped
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        collInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, collInfo, `Collection '${collName}' should exist after conversion`);
        assert.eq(
            true,
            collInfo.options.capped,
            `Collection should be capped after conversion. Actual: ${collInfo.options.capped}`,
        );
        assert.eq(
            cappedSize,
            collInfo.options.size,
            `Capped size should match. Expected: ${cappedSize}, Actual: ${collInfo.options.size}`,
        );
    });

    it("should list collection info after shardCollection", function () {
        // Skip this test if not running on a sharded cluster
        if (!FixtureHelpers.isMongos(testDb)) {
            jsTest.log.info("Skipping shardCollection test: not running on a sharded cluster");
            return;
        }

        // Shard the collection
        assert.commandWorked(
            testDb.adminCommand({
                shardCollection: coll.getFullName(),
                key: {shardKey: 1},
            }),
        );

        // Verify collection exists after sharding
        const collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(
            1,
            collections.length,
            `Collection should exist after sharding. Found ${collections.length} collections: ${tojson(collections)}`,
        );
        const collInfo = collections[0];
        assert.eq(
            collName,
            collInfo.name,
            `Collection name should match. Expected: ${collName}, Actual: ${collInfo.name}`,
        );
    });

    it("should filter collections correctly using filter parameter", () => {
        // Create multiple collections with different properties
        assert.commandWorked(testDb.createCollection("normalColl"));
        const collation = {locale: "fr", strength: 1};
        assert.commandWorked(testDb.createCollection("collatedColl", {collation: collation}));
        const validator = {$jsonSchema: {required: ["validated"], properties: {validated: {bsonType: "bool"}}}};
        assert.commandWorked(testDb.createCollection("validatedColl", {validator: validator}));

        // Test filter by name
        let collections = getCollectionsFromListCollections(testDb, {name: "normalColl"});
        assert.eq(
            1,
            collections.length,
            `Filter by name should return 1 collection. Found ${collections.length} collections: ${tojson(collections.map((c) => c.name))}`,
        );
        assert.eq(
            "normalColl",
            collections[0].name,
            `Filtered collection name should match. Expected: 'normalColl', Actual: ${collections[0].name}`,
        );

        // Test filter by type
        collections = getCollectionsFromListCollections(testDb, {type: "collection"});
        assert.gte(
            collections.length,
            3,
            `Should have at least 3 collections of type 'collection'. Found ${collections.length} collections: ${tojson(collections.map((c) => c.name))}`,
        );

        // Test filter by collation option
        collections = getCollectionsFromListCollections(testDb, {"options.collation.locale": "fr"});
        assert.gte(
            collections.length,
            1,
            `Should have at least 1 collection with French collation. Found ${collections.length} collections: ${tojson(collections.map((c) => c.name))}`,
        );
        const collatedInfo = findCollectionByName(collections, "collatedColl");
        assert.neq(
            undefined,
            collatedInfo,
            `Collated collection should be in filtered results. Available collections: ${tojson(collections.map((c) => c.name))}`,
        );
        assert.eq(
            "fr",
            collatedInfo.options.collation.locale,
            `Filtered collection should have French locale. Actual: ${collatedInfo.options.collation.locale}`,
        );
        assert.eq(
            1,
            collatedInfo.options.collation.strength,
            `Filtered collection should have strength 1. Actual: ${collatedInfo.options.collation.strength}`,
        );
    });

    it("should show collection with collation after createCollection with collation", () => {
        // Create collection with collation
        const collation = {locale: "en_US", strength: 2};
        assert.commandWorked(testDb.createCollection(collName, {collation: collation}));

        // Verify collection has collation
        const collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(1, collections.length, `Collection should exist. Found ${collections.length} collections`);
        const collInfo = collections[0];
        assert.neq(
            undefined,
            collInfo.options.collation,
            `Collection should have collation. Actual options: ${tojson(collInfo.options)}`,
        );
        assert.eq(
            "en_US",
            collInfo.options.collation.locale,
            `Collation locale should match. Expected: 'en_US', Actual: ${collInfo.options.collation.locale}`,
        );
        assert.eq(
            2,
            collInfo.options.collation.strength,
            `Collation strength should match. Expected: 2, Actual: ${collInfo.options.collation.strength}`,
        );
    });

    it("should handle multiple collections with different options", () => {
        // Create several collections with various options
        assert.commandWorked(testDb.createCollection("coll1"));
        assert.commandWorked(testDb.createCollection("coll2", {capped: true, size: 2048}));
        assert.commandWorked(
            testDb.createCollection("coll3", {
                validator: {$jsonSchema: {required: ["id"], properties: {id: {bsonType: "int"}}}},
            }),
        );
        assert.commandWorked(
            testDb.createCollection("coll4", {
                validationLevel: "moderate",
                validationAction: "warn",
            }),
        );

        // Get all collections
        const collections = getCollectionsFromListCollections(testDb);
        const collectionNames = collections.map((c) => c.name);

        // Verify all collections exist
        assert.neq(
            undefined,
            findCollectionByName(collections, "coll1"),
            `coll1 should exist. Available collections: ${tojson(collectionNames)}`,
        );
        assert.neq(
            undefined,
            findCollectionByName(collections, "coll2"),
            `coll2 should exist. Available collections: ${tojson(collectionNames)}`,
        );
        assert.neq(
            undefined,
            findCollectionByName(collections, "coll3"),
            `coll3 should exist. Available collections: ${tojson(collectionNames)}`,
        );
        assert.neq(
            undefined,
            findCollectionByName(collections, "coll4"),
            `coll4 should exist. Available collections: ${tojson(collectionNames)}`,
        );

        // Verify options are correct
        const coll2Info = findCollectionByName(collections, "coll2");
        assert.eq(true, coll2Info.options.capped, `coll2 should be capped. Actual: ${coll2Info.options.capped}`);

        const coll3Info = findCollectionByName(collections, "coll3");
        assert.neq(
            undefined,
            coll3Info.options.validator,
            `coll3 should have validator. Actual options: ${tojson(coll3Info.options)}`,
        );

        const coll4Info = findCollectionByName(collections, "coll4");
        assert.eq(
            "moderate",
            coll4Info.options.validationLevel,
            `coll4 should have moderate validation level. Actual: ${coll4Info.options.validationLevel}`,
        );
        assert.eq(
            "warn",
            coll4Info.options.validationAction,
            `coll4 should have warn validation action. Actual: ${coll4Info.options.validationAction}`,
        );
    });

    it("should list view correctly after createView and not list it after drop", () => {
        // Create a source collection
        assert.commandWorked(testDb.createCollection(collName));

        // Verify only the collection exists initially
        let collections = getCollectionsFromListCollections(testDb);
        let collInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, collInfo, `Collection '${collName}' should exist`);
        assert.eq("collection", collInfo.type, `Type should be 'collection'. Actual: ${collInfo.type}`);

        // Create a view
        const viewName = "viewTest";
        const pipeline = [{$match: {status: "active"}}];
        assert.commandWorked(testDb.createView(viewName, collName, pipeline));

        // Verify view exists with correct properties
        collections = getCollectionsFromListCollections(testDb);
        const viewInfo = findCollectionByName(collections, viewName);
        assert.neq(
            undefined,
            viewInfo,
            `View '${viewName}' should exist. Available: ${tojson(collections.map((c) => c.name))}`,
        );
        assert.eq("view", viewInfo.type, `Type should be 'view'. Actual: ${viewInfo.type}`);
        assert.eq(
            collName,
            viewInfo.options.viewOn,
            `viewOn should be '${collName}'. Actual: ${viewInfo.options.viewOn}`,
        );
        assert.docEq(
            pipeline,
            viewInfo.options.pipeline,
            `Pipeline should match. Expected: ${tojson(pipeline)}, Actual: ${tojson(viewInfo.options.pipeline)}`,
        );
        assert.eq(true, viewInfo.info.readOnly, `View should be readOnly. Actual: ${viewInfo.info.readOnly}`);
        assert.eq(undefined, viewInfo.idIndex, `View should not have idIndex. Actual: ${tojson(viewInfo.idIndex)}`);

        // Drop the view
        assert.commandWorked(testDb.runCommand({drop: viewName}));

        // Verify view no longer exists
        collections = getCollectionsFromListCollections(testDb);
        assert.eq(
            undefined,
            findCollectionByName(collections, viewName),
            `View '${viewName}' should not exist after drop. Available: ${tojson(collections.map((c) => c.name))}`,
        );

        // Verify source collection still exists
        assert.neq(
            undefined,
            findCollectionByName(collections, collName),
            `Source collection '${collName}' should still exist`,
        );
        assert.eq(
            "collection",
            findCollectionByName(collections, collName).type,
            `Source should be 'collection'. Actual: ${findCollectionByName(collections, collName).type}`,
        );
    });

    it("should update view definition after collMod changes pipeline", () => {
        // Create source collection and view
        assert.commandWorked(testDb.createCollection(collName));
        const viewName = "viewModifyTest";
        const initialPipeline = [{$match: {status: "active"}}];
        assert.commandWorked(testDb.createView(viewName, collName, initialPipeline));

        // Verify initial view definition
        let collections = getCollectionsFromListCollections(testDb);
        let viewInfo = findCollectionByName(collections, viewName);
        assert.neq(undefined, viewInfo, `View '${viewName}' should exist`);
        assert.eq(
            collName,
            viewInfo.options.viewOn,
            `viewOn should be '${collName}'. Actual: ${viewInfo.options.viewOn}`,
        );
        assert.docEq(
            initialPipeline,
            viewInfo.options.pipeline,
            `Initial pipeline should match. Expected: ${tojson(initialPipeline)}, Actual: ${tojson(viewInfo.options.pipeline)}`,
        );

        // Use collMod to change the pipeline
        const newPipeline = [{$match: {status: "inactive"}}, {$project: {name: 1, status: 1}}];
        assert.commandWorked(
            testDb.runCommand({
                collMod: viewName,
                viewOn: collName,
                pipeline: newPipeline,
            }),
        );

        // Verify pipeline has been updated
        collections = getCollectionsFromListCollections(testDb);
        viewInfo = findCollectionByName(collections, viewName);
        assert.neq(undefined, viewInfo, `View '${viewName}' should exist after collMod`);
        assert.docEq(
            newPipeline,
            viewInfo.options.pipeline,
            `Pipeline should be updated. Expected: ${tojson(newPipeline)}, Actual: ${tojson(viewInfo.options.pipeline)}`,
        );
    });

    it("should update view definition after collMod changes viewOn", () => {
        // Create two source collections and a view
        assert.commandWorked(testDb.createCollection(collName));
        const sourceCollName2 = "sourceCollection2";
        assert.commandWorked(testDb.createCollection(sourceCollName2));

        const viewName = "viewChangeSourceTest";
        const pipeline = [{$match: {active: true}}];
        assert.commandWorked(testDb.createView(viewName, collName, pipeline));

        // Verify initial view points to first collection
        let collections = getCollectionsFromListCollections(testDb);
        let viewInfo = findCollectionByName(collections, viewName);
        assert.neq(undefined, viewInfo, `View '${viewName}' should exist`);
        assert.eq(
            collName,
            viewInfo.options.viewOn,
            `viewOn should be '${collName}'. Actual: ${viewInfo.options.viewOn}`,
        );

        // Use collMod to change the viewOn to second collection
        assert.commandWorked(
            testDb.runCommand({
                collMod: viewName,
                viewOn: sourceCollName2,
                pipeline: pipeline,
            }),
        );

        // Verify viewOn has been updated
        collections = getCollectionsFromListCollections(testDb);
        viewInfo = findCollectionByName(collections, viewName);
        assert.neq(undefined, viewInfo, `View '${viewName}' should exist after collMod`);
        assert.eq(
            sourceCollName2,
            viewInfo.options.viewOn,
            `viewOn should be updated to '${sourceCollName2}'. Actual: ${viewInfo.options.viewOn}`,
        );
        assert.docEq(
            pipeline,
            viewInfo.options.pipeline,
            `Pipeline should remain unchanged. Expected: ${tojson(pipeline)}, Actual: ${tojson(viewInfo.options.pipeline)}`,
        );
    });

    it("should list view with collation after createView with collation", () => {
        // Create source collection
        assert.commandWorked(testDb.createCollection(collName));

        // Create view with collation
        const viewName = "viewWithCollation";
        const pipeline = [{$match: {name: {$exists: true}}}];
        const collation = {locale: "en", strength: 2};
        assert.commandWorked(
            testDb.runCommand({
                create: viewName,
                viewOn: collName,
                pipeline: pipeline,
                collation: collation,
            }),
        );

        // Verify view has collation
        const collections = getCollectionsFromListCollections(testDb);
        const viewInfo = findCollectionByName(collections, viewName);
        assert.neq(undefined, viewInfo, `View '${viewName}' should exist`);
        assert.eq("view", viewInfo.type, `Type should be 'view'. Actual: ${viewInfo.type}`);
        assert.neq(
            undefined,
            viewInfo.options.collation,
            `View should have collation. Options: ${tojson(viewInfo.options)}`,
        );
        assert.eq(
            "en",
            viewInfo.options.collation.locale,
            `Collation locale should be 'en'. Actual: ${viewInfo.options.collation.locale}`,
        );
        assert.eq(
            2,
            viewInfo.options.collation.strength,
            `Collation strength should be 2. Actual: ${viewInfo.options.collation.strength}`,
        );
    });

    it("should distinguish between collections and views in listCollections", () => {
        // Create collections and views
        assert.commandWorked(testDb.createCollection("regularColl1"));
        assert.commandWorked(testDb.createCollection("regularColl2"));
        assert.commandWorked(testDb.createView("view1", "regularColl1", [{$match: {x: 1}}]));
        assert.commandWorked(testDb.createView("view2", "regularColl2", [{$match: {y: 1}}]));

        // Get all collections and views
        const allItems = getCollectionsFromListCollections(testDb);

        // Filter by type
        const collections = allItems.filter((item) => item.type === "collection");
        const views = allItems.filter((item) => item.type === "view");

        // Verify we have the correct counts
        assert.gte(collections.length, 2, `Should have at least 2 collections. Found: ${collections.length}`);
        assert.gte(views.length, 2, `Should have at least 2 views. Found: ${views.length}`);

        // Verify specific collections exist
        assert.neq(undefined, findCollectionByName(collections, "regularColl1"), `regularColl1 should be a collection`);
        assert.neq(undefined, findCollectionByName(collections, "regularColl2"), `regularColl2 should be a collection`);

        // Verify specific views exist
        assert.neq(undefined, findCollectionByName(views, "view1"), `view1 should be a view`);
        assert.neq(undefined, findCollectionByName(views, "view2"), `view2 should be a view`);

        // Verify views have readOnly flag and no idIndex
        views.forEach((view) => {
            assert.eq(
                true,
                view.info.readOnly,
                `View '${view.name}' should be readOnly. Actual: ${view.info.readOnly}`,
            );
            assert.eq(
                undefined,
                view.idIndex,
                `View '${view.name}' should not have idIndex. Actual: ${tojson(view.idIndex)}`,
            );
        });

        // Verify collections have idIndex and are not readOnly
        collections.forEach((coll) => {
            assert.eq(
                false,
                coll.info.readOnly,
                `Collection '${coll.name}' should not be readOnly. Actual: ${coll.info.readOnly}`,
            );
        });
    });

    it("should not list views or collections after dropDatabase", () => {
        // Create collections and views
        assert.commandWorked(testDb.createCollection("coll1"));
        assert.commandWorked(testDb.createCollection("coll2"));
        assert.commandWorked(testDb.createView("view1", "coll1", []));
        assert.commandWorked(testDb.createView("view2", "coll2", []));

        // Verify collections and views exist
        let allItems = getCollectionsFromListCollections(testDb);
        assert.gte(allItems.length, 4, `Should have at least 4 items. Found ${allItems.length}`);

        // Drop database
        assert.commandWorked(testDb.dropDatabase());

        // Verify no collections or views exist
        allItems = getCollectionsFromListCollections(testDb);
        assert.eq(
            0,
            allItems.length,
            `Should have no collections or views after dropDatabase. Found ${allItems.length}: ${tojson(allItems.map((c) => c.name))}`,
        );
    });

    it("should list timeseries collection correctly after createCollection with timeseries options", () => {
        // Create timeseries collection
        const collName = "tsCollTest";
        const timeseriesOptions = {
            timeField: "timestamp",
            metaField: "metadata",
            granularity: "hours",
        };
        assert.commandWorked(testDb.createCollection(collName, {timeseries: timeseriesOptions}));

        // Verify timeseries collection exists with correct options
        const collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(1, collections.length, `Timeseries collection should exist. Found ${collections.length} collections`);
        const tsCollInfo = collections[0];
        assert.eq(
            collName,
            tsCollInfo.name,
            `Collection name should match. Expected: ${collName}, Actual: ${tsCollInfo.name}`,
        );
        assert.eq("timeseries", tsCollInfo.type, `Type should be 'timeseries'. Actual: ${tsCollInfo.type}`);
        assert.neq(
            undefined,
            tsCollInfo.options.timeseries,
            `Should have timeseries options. Options: ${tojson(tsCollInfo.options)}`,
        );
        assert.eq(
            "timestamp",
            tsCollInfo.options.timeseries.timeField,
            `timeField should be 'timestamp'. Actual: ${tsCollInfo.options.timeseries.timeField}`,
        );
        assert.eq(
            "metadata",
            tsCollInfo.options.timeseries.metaField,
            `metaField should be 'metadata'. Actual: ${tsCollInfo.options.timeseries.metaField}`,
        );
        assert.eq(
            "hours",
            tsCollInfo.options.timeseries.granularity,
            `granularity should be 'hours'. Actual: ${tsCollInfo.options.timeseries.granularity}`,
        );
    });

    it("should list timeseries collection with minimal options after createCollection and not list it after drop", () => {
        // Create timeseries collection with only required timeField
        const collName = "tsCollMinimal";
        assert.commandWorked(
            testDb.createCollection(collName, {
                timeseries: {timeField: "ts"},
            }),
        );

        // Verify timeseries collection exists
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(1, collections.length, `Timeseries collection should exist. Found ${collections.length} collections`);
        const tsCollInfo = collections[0];
        assert.eq("timeseries", tsCollInfo.type, `Type should be 'timeseries'. Actual: ${tsCollInfo.type}`);
        assert.eq(
            "ts",
            tsCollInfo.options.timeseries.timeField,
            `timeField should be 'ts'. Actual: ${tsCollInfo.options.timeseries.timeField}`,
        );
        // metaField is optional, so it might not be present
        assert.neq(
            undefined,
            tsCollInfo.options.timeseries.granularity,
            `granularity should have a default value. Actual: ${tsCollInfo.options.timeseries.granularity}`,
        );

        // Drop the timeseries collection
        const tsColl = testDb.getCollection(collName);
        dropCollection(tsColl);

        // Verify timeseries collection no longer exists
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        assert.eq(
            0,
            collections.length,
            `Timeseries collection should not exist after drop. Found ${collections.length} collections: ${tojson(collections)}`,
        );
    });

    it("should update timeseries collection options after collMod changes granularity", () => {
        // Create timeseries collection with initial granularity
        const collName = "tsCollModGranularity";
        assert.commandWorked(
            testDb.createCollection(collName, {
                timeseries: {
                    timeField: "timestamp",
                    granularity: "seconds",
                },
            }),
        );

        // Verify initial granularity
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        let tsCollInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, tsCollInfo, `Timeseries collection '${collName}' should exist`);
        assert.eq(
            "seconds",
            tsCollInfo.options.timeseries.granularity,
            `Initial granularity should be 'seconds'. Actual: ${tsCollInfo.options.timeseries.granularity}`,
        );

        // Use collMod to change granularity
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                timeseries: {granularity: "minutes"},
            }),
        );

        // Verify granularity has been updated
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        tsCollInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, tsCollInfo, `Timeseries collection '${collName}' should exist after collMod`);
        assert.eq(
            "minutes",
            tsCollInfo.options.timeseries.granularity,
            `Granularity should be updated to 'minutes'. Actual: ${tsCollInfo.options.timeseries.granularity}`,
        );
    });

    it("should update timeseries collection options after collMod changes bucketMaxSpanSeconds and bucketRoundingSeconds", () => {
        // Create timeseries collection with initial bucketMaxSpanSeconds and bucketRoundingSeconds (without granularity)
        const collName = "tsCollModBucketSpan";
        const initialBucketMaxSpan = 3600; // 1 hour
        const initialBucketRounding = 3600; // 1 hour
        assert.commandWorked(
            testDb.createCollection(collName, {
                timeseries: {
                    timeField: "timestamp",
                    bucketMaxSpanSeconds: initialBucketMaxSpan,
                    bucketRoundingSeconds: initialBucketRounding,
                },
            }),
        );

        // Verify initial bucketMaxSpanSeconds and bucketRoundingSeconds
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        let tsCollInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, tsCollInfo, `Timeseries collection '${collName}' should exist`);
        assert.eq(
            initialBucketMaxSpan,
            tsCollInfo.options.timeseries.bucketMaxSpanSeconds,
            `Initial bucketMaxSpanSeconds should be ${initialBucketMaxSpan}. Actual: ${tsCollInfo.options.timeseries.bucketMaxSpanSeconds}`,
        );
        assert.eq(
            initialBucketRounding,
            tsCollInfo.options.timeseries.bucketRoundingSeconds,
            `Initial bucketRoundingSeconds should be ${initialBucketRounding}. Actual: ${tsCollInfo.options.timeseries.bucketRoundingSeconds}`,
        );

        // Use collMod to change both bucketMaxSpanSeconds and bucketRoundingSeconds
        const newBucketMaxSpan = 7200; // 2 hours
        const newBucketRounding = 7200; // 2 hours
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                timeseries: {
                    bucketMaxSpanSeconds: newBucketMaxSpan,
                    bucketRoundingSeconds: newBucketRounding,
                },
            }),
        );

        // Verify both values have been updated
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        tsCollInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, tsCollInfo, `Timeseries collection '${collName}' should exist after collMod`);
        assert.eq(
            newBucketMaxSpan,
            tsCollInfo.options.timeseries.bucketMaxSpanSeconds,
            `bucketMaxSpanSeconds should be updated to ${newBucketMaxSpan}. Actual: ${tsCollInfo.options.timeseries.bucketMaxSpanSeconds}`,
        );
        assert.eq(
            newBucketRounding,
            tsCollInfo.options.timeseries.bucketRoundingSeconds,
            `bucketRoundingSeconds should be updated to ${newBucketRounding}. Actual: ${tsCollInfo.options.timeseries.bucketRoundingSeconds}`,
        );
    });

    it("should update timeseries collection expireAfterSeconds after collMod", () => {
        // Create timeseries collection with initial expireAfterSeconds
        const initialExpire = 3600; // 1 hour
        assert.commandWorked(
            testDb.createCollection(collName, {
                timeseries: {timeField: "timestamp"},
                expireAfterSeconds: initialExpire,
            }),
        );

        // Verify initial expireAfterSeconds
        let collections = getCollectionsFromListCollections(testDb, {name: collName});
        let tsCollInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, tsCollInfo, `Timeseries collection '${collName}' should exist`);
        assert.eq(
            initialExpire,
            tsCollInfo.options.expireAfterSeconds,
            `Initial expireAfterSeconds should be ${initialExpire}. Actual: ${tsCollInfo.options.expireAfterSeconds}`,
        );

        // Use collMod to change expireAfterSeconds
        const newExpire = 7200; // 2 hours
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                expireAfterSeconds: newExpire,
            }),
        );

        // Verify expireAfterSeconds has been updated
        collections = getCollectionsFromListCollections(testDb, {name: collName});
        tsCollInfo = findCollectionByName(collections, collName);
        assert.neq(undefined, tsCollInfo, `Timeseries collection '${collName}' should exist after collMod`);
        assert.eq(
            newExpire,
            tsCollInfo.options.expireAfterSeconds,
            `expireAfterSeconds should be updated to ${newExpire}. Actual: ${tsCollInfo.options.expireAfterSeconds}`,
        );
    });

    it("should list timeseries collection and its underlying bucket collection, and remove both after drop", () => {
        if (FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections")) {
            jsTest.log.info(
                "Skipping timeseries collection and its underlying bucket collection test because CreateViewlessTimeseriesCollections is enabled",
            );
            return;
        }

        // Create timeseries collection
        assert.commandWorked(
            testDb.createCollection(collName, {
                timeseries: {timeField: "timestamp"},
            }),
        );

        // Get all collections including system collections
        let allCollections = getCollectionsFromListCollections(testDb);

        // Verify timeseries collection exists
        const tsCollInfo = findCollectionByName(allCollections, collName);
        assert.neq(undefined, tsCollInfo, `Timeseries collection '${collName}' should exist`);
        assert.eq("timeseries", tsCollInfo.type, `Type should be 'timeseries'. Actual: ${tsCollInfo.type}`);

        // The underlying bucket collection (system.buckets.<collName>) should exist
        const bucketCollName = `system.buckets.${collName}`;
        const bucketCollInfo = findCollectionByName(allCollections, bucketCollName);
        assert.neq(
            undefined,
            bucketCollInfo,
            `Bucket collection '${bucketCollName}' should exist. Available: ${tojson(allCollections.map((c) => c.name))}`,
        );
        assert.eq(
            "collection",
            bucketCollInfo.type,
            `Bucket collection type should be 'collection'. Actual: ${bucketCollInfo.type}`,
        );

        // Drop timeseries collections and bucket collection
        const tsColl = testDb.getCollection(collName);
        dropCollection(tsColl);

        // Verify both collections are removed
        allCollections = getCollectionsFromListCollections(testDb);
        assert.eq(
            undefined,
            findCollectionByName(allCollections, collName),
            `Timeseries collection should not exist after drop. Available: ${tojson(allCollections.map((c) => c.name))}`,
        );
        assert.eq(
            undefined,
            findCollectionByName(allCollections, bucketCollName),
            `Bucket collection should not exist after drop. Available: ${tojson(allCollections.map((c) => c.name))}`,
        );
    });
});
