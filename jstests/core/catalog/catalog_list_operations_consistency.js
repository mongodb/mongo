/**
 * Tests the consistency of the results obtained from the $listCatalog aggregation stage
 * with the results from listCollections / listIndexes commands.
 *
 * @tags: [
 *   requires_capped,
 *   requires_timeseries,
 *   # $listCatalog does not include the tenant prefix in its results.
 *   command_not_supported_in_serverless,
 * ]
 */
import {
    assertCatalogListOperationsConsistencyForCollection
} from "jstests/libs/catalog_list_operations_consistency_validator.js";

// Validate catalog list operations consistency after each command,
// so that if an inconsistency is introduced, we fail immediately.
function createCollectionAndCheckConsistency(db, name, options) {
    db[name].drop();
    assert.commandWorked(db.createCollection(name, options));
    assertCatalogListOperationsConsistencyForCollection(db[name]);
}

function createViewAndCheckConsistency(db, view, source, pipeline, collation) {
    db[view].drop();
    assert.commandWorked(db.createView(view, source, pipeline, collation));
    assertCatalogListOperationsConsistencyForCollection(db[view]);
}

function createIndexAndCheckConsistency(collection, keys, options) {
    assert.commandWorked(collection.createIndex(keys, options));
    assertCatalogListOperationsConsistencyForCollection(collection);
}

/**
 * Collection test cases.
 */
createCollectionAndCheckConsistency(db, "collection_simple");

createCollectionAndCheckConsistency(db, "collection_capped", {
    capped: true,
    size: 1048576,
    max: 1024,
});

createCollectionAndCheckConsistency(
    db, "collection_images", {changeStreamPreAndPostImages: {enabled: true}});

createCollectionAndCheckConsistency(db, "collection_clustered", {
    clusteredIndex: {"key": {_id: 1}, "unique": true, "name": "clustered index"},
    expireAfterSeconds: 600,
});

createCollectionAndCheckConsistency(db, "collection_wiredtiger", {
    storageEngine: {wiredTiger: {configString: "block_compressor=zlib"}}
});

createCollectionAndCheckConsistency(db, "collection_validator", {
    validator: {
        $jsonSchema: {
            bsonType: "object",
            required: ["a", "b", "c"],
        }
    },
    validationLevel: "moderate",
});

/**
 * View test cases.
 */
createViewAndCheckConsistency(db, "view_simple", "collection_simple", [{$match: {a: 100}}]);
createViewAndCheckConsistency(
    db, "view_collation", "collection_simple", [{$match: {a: 100}}], {collation: {locale: "fr"}});

/**
 * Timeseries test cases.
 */
createCollectionAndCheckConsistency(db, "timeseries_simple", {timeseries: {timeField: 't'}});

createCollectionAndCheckConsistency(db, "timeseries_complex", {
    timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"},
    collation: {
        locale: "fr",
    },
    expireAfterSeconds: 600,
    storageEngine: {wiredTiger: {configString: "block_compressor=snappy"}}
});

assert.commandWorked(
    db.runCommand({collMod: "timeseries_complex", timeseriesBucketsMayHaveMixedSchemaData: true}));
assertCatalogListOperationsConsistencyForCollection(db.timeseries_complex);

/**
 * Index test cases.
 */
createIndexAndCheckConsistency(db.collection_simple, {fSimple: -1});
createIndexAndCheckConsistency(db.collection_simple, {fCompound1: -1, fCompound2: 1});
createIndexAndCheckConsistency(db.collection_simple, {fUnique: 1}, {unique: true});
createIndexAndCheckConsistency(db.collection_simple, {fSparse: 1}, {sparse: true});
createIndexAndCheckConsistency(db.collection_simple, {fSparseNonBool: 1}, {sparse: 123.45});
createIndexAndCheckConsistency(db.collection_simple, {fUnique: 1}, {unique: true});
createIndexAndCheckConsistency(db.collection_simple, {fTtl: 1}, {expireAfterSeconds: 123});
createIndexAndCheckConsistency(db.collection_simple, {fTtlNumber: 1}, {expireAfterSeconds: 123.45});
createIndexAndCheckConsistency(db.collection_simple, {fWiredTiger: 1}, {
    storageEngine: {wiredTiger: {configString: "app_metadata=(test=123)"}}
});
createIndexAndCheckConsistency(db.collection_simple, {fHidden: 1}, {hidden: true});
createIndexAndCheckConsistency(db.collection_simple, {fNamed: 1}, {name: "namedindex"});
createIndexAndCheckConsistency(db.collection_simple, {fCollation: 1}, {collation: {locale: 'es'}});
createIndexAndCheckConsistency(db.collection_simple, {"$**": 1});
createIndexAndCheckConsistency(db.collection_simple, {fText: "text"});
createIndexAndCheckConsistency(db.collection_simple, {fHashed: "hashed"});
createIndexAndCheckConsistency(db.collection_simple, {f2d: "2d"});
createIndexAndCheckConsistency(db.collection_simple, {f2dNonIntBits: "2d"}, {bits: 24.68});
createIndexAndCheckConsistency(db.collection_simple, {f2dSphere: "2dsphere"});
createIndexAndCheckConsistency(db.timeseries_simple, {"timestamp": 1});

// TODO(SERVER-97084): Remove when options for index plugins are denied in basic indexes.
createIndexAndCheckConsistency(db.collection_simple, {fUnrelatedIndexPluginOptions: 1}, {
    textIndexVersion: 3,
    '2dsphereIndexVersion': 3,
    bits: 26,
    min: -180,
    max: 180,
});
