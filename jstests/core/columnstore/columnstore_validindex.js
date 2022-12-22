/**
 * Tests parsing and validation of columnstore indexes.
 * @tags: [
 *   requires_fcv_63,
 *   # Uses index building in background.
 *   requires_background_index,
 *   # Columnstore tests set server parameters to disable columnstore query planning heuristics -
 *   # 1) server parameters are stored in-memory only so are not transferred onto the recipient,
 *   # 2) server parameters may not be set in stepdown passthroughs because it is a command that may
 *   #      return different values after a failover
 *   tenant_migration_incompatible,
 *   does_not_support_stepdowns,
 *   not_allowed_with_security_token,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/index_catalog_helpers.js");     // For "IndexCatalogHelpers."
load("jstests/libs/collection_drop_recreate.js");  // For "assertDropCollection."
load("jstests/libs/columnstore_util.js");          // For "setUpServerForColumnStoreIndexTest."

if (!setUpServerForColumnStoreIndexTest(db)) {
    return;
}

const kCollectionName = "columnstore_validindex";
const coll = db.getCollection(kCollectionName);

const kIndexName = "columnstore_index";
const kKeyPattern = {
    "$**": "columnstore"
};

// Can create a valid columnstore index.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(coll, kKeyPattern, {name: kIndexName});

// Can create a columnstore index with foreground & background construction.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, kKeyPattern, {background: false, name: kIndexName});
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, kKeyPattern, {background: true, name: kIndexName});

// Test that you cannot create a columnstore index with a collation - either with the argument or
// because the collection has a default collation specified.
assert.commandFailedWithCode(
    coll.createIndex(kKeyPattern, {collation: {locale: "fr"}, name: kIndexName}),
    ErrorCodes.CannotCreateIndex);

const collationCollName = "columnstore_collation";
const collationColl = db[collationCollName];
assertDropCollection(db, collationCollName);
assert.commandWorked(db.createCollection(collationCollName, {collation: {locale: "fr"}}));
assert.commandFailedWithCode(collationColl.createIndex(kKeyPattern), ErrorCodes.CannotCreateIndex);

// Can create a valid columnstore index with subpaths.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"a.$**": "columnstore"}, {name: kIndexName});

// Cannot create a columnstore index with any of the following options which could apply to other
// indexes.
function assertCannotUseOptions(options) {
    assert.commandFailedWithCode(coll.createIndex(kKeyPattern, options),
                                 ErrorCodes.CannotCreateIndex);
}
coll.dropIndexes();
assertCannotUseOptions({partialFilterExpression: {a: {"$gt": 0}}});
assertCannotUseOptions({sparse: true});
assertCannotUseOptions({expireAfterSeconds: 3000});
assertCannotUseOptions({v: 0});
assertCannotUseOptions({v: 1});
assertCannotUseOptions({unique: true});

// Can create a columnstore index with an inclusion projection.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, kKeyPattern, {columnstoreProjection: {a: 1, b: 1, c: 1}, name: kIndexName});

// Can create a columnstore index with an exclusion projection.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, kKeyPattern, {columnstoreProjection: {a: 0, b: 0, c: 0}, name: kIndexName});

// Can include _id in an exclusion.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, kKeyPattern, {columnstoreProjection: {_id: 1, a: 0, b: 0, c: 0}, name: kIndexName});

// Can exclude _id in an inclusion.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, kKeyPattern, {columnstoreProjection: {_id: 0, a: 1, b: 1, c: 1}, name: kIndexName});

// Cannot create column store index with wildcardProjection.
assert.commandFailedWithCode(
    coll.createIndex(kKeyPattern, {wildcardProjection: {a: 1, b: 1}, name: kIndexName}),
    ErrorCodes.BadValue);

// Cannot mix wildcardProjection with columnstoreProjection.
assert.commandFailedWithCode(
    coll.createIndex(
        kKeyPattern,
        {columnstoreProjection: {a: 1, b: 1}, wildcardProjection: {a: 1, b: 1}, name: kIndexName}),
    ErrorCodes.BadValue);

// Cannot create a columnstore index with a different capitalization.
coll.dropIndexes();
function assertUnrecognizedName(name) {
    assert.commandFailedWithCode(coll.createIndex({"$**": name}), ErrorCodes.CannotCreateIndex);
}
assertUnrecognizedName("Columnstore");
assertUnrecognizedName("ColumnStore");
assertUnrecognizedName("columnStore");
assertUnrecognizedName("COLUMNSTORE");
assertUnrecognizedName("column_store");

// Cannot create a compound columnstore index.
assert.commandFailedWithCode(coll.createIndex({"$**": "columnstore", "a": 1}),
                             ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndex({"a": 1, "$**": "columnstore"}),
                             ErrorCodes.CannotCreateIndex);

// Cannot create an columnstore index with an invalid spec.
assert.commandFailedWithCode(coll.createIndex({"a.$**.$**": "columnstore"}),
                             ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndex({"$**.$**": "columnstore"}),
                             ErrorCodes.CannotCreateIndex);

// Cannot create a columnstore index with mixed inclusion exclusion.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, kKeyPattern, {name: kIndexName, columnstoreProjection: {a: 1, b: 0}}),
    31254);

// Cannot create a columnstore index with computed fields.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, kKeyPattern, {name: kIndexName, columnstoreProjection: {a: 1, b: "string"}}),
    51271);

// Cannot create a columnstore index with an empty projection.
assert.commandFailedWithCode(IndexCatalogHelpers.createSingleIndex(
                                 coll, kKeyPattern, {name: kIndexName, columnstoreProjection: {}}),
                             ErrorCodes.FailedToParse);

// Cannot create another index type with "columnstoreProjection" projection.
function assertCannotUseColumnstoreProjection(indexKeyPattern) {
    assert.commandFailedWithCode(
        IndexCatalogHelpers.createSingleIndex(
            coll, indexKeyPattern, {name: kIndexName, columnstoreProjection: {a: 1, b: 1}}),
        ErrorCodes.InvalidIndexSpecificationOption);
}
assertCannotUseColumnstoreProjection({a: 1});
assertCannotUseColumnstoreProjection({"$**": 1});
assertCannotUseColumnstoreProjection({"$**": "text"});

// Cannot create a columnstore index with a non-object "columnstoreProjection".
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"a.$**": "columnstore"}, {name: kIndexName, columnstoreProjection: "string"}),
    ErrorCodes.TypeMismatch);

// Cannot exclude a subfield of _id in an inclusion.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(coll, {"_id.id": 0, a: 1, b: 1, c: 1}),
    ErrorCodes.CannotCreateIndex);

// Cannot include a subfield of _id in an exclusion.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(coll, {"_id.id": 1, a: 0, b: 0, c: 0}),
    ErrorCodes.CannotCreateIndex);

// Cannot specify both a subpath and a projection.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"a.$**": "columnstore"}, {name: kIndexName, columnstoreProjection: {a: 1}}),
    ErrorCodes.FailedToParse);
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"a.$**": "columnstore"}, {name: kIndexName, columnstoreProjection: {b: 0}}),
    ErrorCodes.FailedToParse);

// Test that you cannot create a columnstore index on a clustered collection.
const clusteredCollName = "columnstore_clustered";
const clusteredColl = db[clusteredCollName];
assertDropCollection(db, clusteredCollName);
assert.commandWorked(
    db.runCommand({create: clusteredCollName, clusteredIndex: {key: {_id: 1}, unique: true}}));
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(clusteredColl, kKeyPattern, {name: kIndexName}),
    ErrorCodes.InvalidOptions);

// Test that you cannot cluster a collection using a columnstore index.
// Need to specify 'unique' field.
assertDropCollection(db, clusteredCollName);
assert.commandFailedWithCode(
    db.runCommand({create: clusteredCollName, clusteredIndex: {key: kKeyPattern}}), 40414);
// Even with unique it still should not work.
assert.commandFailedWithCode(
    db.runCommand({create: clusteredCollName, clusteredIndex: {key: kKeyPattern, unique: true}}),
    ErrorCodes.InvalidIndexSpecificationOption);
assert.commandFailedWithCode(
    db.runCommand({create: clusteredCollName, clusteredIndex: {key: kKeyPattern, unique: false}}),
    5979700);
})();
