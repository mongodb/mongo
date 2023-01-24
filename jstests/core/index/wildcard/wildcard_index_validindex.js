/**
 * Tests parsing and validation of wildcard indexes.
 * @tags: [
 *   # Uses index building in background
 *   requires_background_index,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/index_catalog_helpers.js");     // For "IndexCatalogHelpers."
load("jstests/libs/collection_drop_recreate.js");  // For "assertDropCollection."
load("jstests/libs/feature_flag_util.js");         // For "FeatureFlagUtil"

const kCollectionName = "wildcard_validindex";
const coll = db.getCollection(kCollectionName);

const kIndexName = "wildcard_validindex";

// TODO SERVER-68303: Remove the feature flag and update corresponding tests.
const allowCompoundWildcardIndexes = FeatureFlagUtil.isEnabled(db, "CompoundWildcardIndexes");

// Can create a valid wildcard index.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(coll, {"$**": 1}, {name: kIndexName});

// Can create a valid wildcard index with subpaths.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(coll, {"a.$**": 1}, {name: kIndexName});

// Can create a wildcard index with partialFilterExpression.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": 1}, {name: kIndexName, partialFilterExpression: {a: {"$gt": 0}}});

// Can create a wildcard index with foreground & background construction.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": 1}, {background: false, name: kIndexName});
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": 1}, {background: true, name: kIndexName});

// Can create a wildcard index with index level collation.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": 1}, {collation: {locale: "fr"}, name: kIndexName});

// Can create a wildcard index with an inclusion projection.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": 1}, {wildcardProjection: {a: 1, b: 1, c: 1}, name: kIndexName});
// Can create a wildcard index with an exclusion projection.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": 1}, {wildcardProjection: {a: 0, b: 0, c: 0}, name: kIndexName});
// Can include _id in an exclusion.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": 1}, {wildcardProjection: {_id: 1, a: 0, b: 0, c: 0}, name: kIndexName});
// Can exclude _id in an exclusion.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": 1}, {wildcardProjection: {_id: 0, a: 1, b: 1, c: 1}, name: kIndexName});

// Cannot create a wildcard index with a non-positive numeric key value.
coll.dropIndexes();
assert.commandFailedWithCode(coll.createIndex({"$**": 0}), ErrorCodes.CannotCreateIndex);
if (!allowCompoundWildcardIndexes) {
    // TODO SERVER-68303: Update the tests. Following createIndex commands should be valid.
    // Negative value is allowed if compound wildcard indexes feature flag is set.
    assert.commandFailedWithCode(coll.createIndex({"$**": -1}), ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({"$**": -2}), ErrorCodes.CannotCreateIndex);
}

// Cannot create a wildcard index with sparse option.
assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {sparse: true}),
                             ErrorCodes.CannotCreateIndex);

// Cannot create a wildcard index with a v0 or v1 index.
assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {v: 0}), ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {v: 1}), ErrorCodes.CannotCreateIndex);

// Cannot create a unique index.
assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {unique: true}),
                             ErrorCodes.CannotCreateIndex);

// Cannot create a hashed wildcard index.
assert.commandFailedWithCode(coll.createIndex({"$**": "hashed"}), ErrorCodes.CannotCreateIndex);

// Cannot create a TTL wildcard index.
assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {expireAfterSeconds: 3600}),
                             ErrorCodes.CannotCreateIndex);

// Cannot create a geoSpatial wildcard index.
assert.commandFailedWithCode(coll.createIndex({"$**": "2dsphere"}), ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndex({"$**": "2d"}), ErrorCodes.CannotCreateIndex);

// Cannot create a text wildcard index using single sub-path syntax.
assert.commandFailedWithCode(coll.createIndex({"a.$**": "text"}), ErrorCodes.CannotCreateIndex);

// Cannot specify plugin by string.
assert.commandFailedWithCode(coll.createIndex({"a": "wildcard"}),
                             [ErrorCodes.CannotCreateIndex, 7246202]);
assert.commandFailedWithCode(coll.createIndex({"$**": "wildcard"}), ErrorCodes.CannotCreateIndex);

// Cannot create a compound wildcard index.
if (!allowCompoundWildcardIndexes) {
    assert.commandFailedWithCode(coll.createIndex({"$**": 1, "a": 1}),
                                 ErrorCodes.CannotCreateIndex);
    assert.commandFailedWithCode(coll.createIndex({"a": 1, "$**": 1}),
                                 ErrorCodes.CannotCreateIndex);
}

// Cannot create an wildcard index with an invalid spec.
assert.commandFailedWithCode(coll.createIndex({"a.$**.$**": 1}), ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndex({"$**.$**": 1}), ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndex({"$**": "hello"}), ErrorCodes.CannotCreateIndex);

// Cannot create an wildcard index with mixed inclusion exclusion.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"$**": 1}, {name: kIndexName, wildcardProjection: {a: 1, b: 0}}),
    31254);
// Cannot create an wildcard index with computed fields.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"$**": 1}, {name: kIndexName, wildcardProjection: {a: 1, b: "string"}}),
    51271);
// Cannot create an wildcard index with an empty projection.
assert.commandFailedWithCode(IndexCatalogHelpers.createSingleIndex(
                                 coll, {"$**": 1}, {name: kIndexName, wildcardProjection: {}}),
                             ErrorCodes.FailedToParse);
// Cannot create another index type with "wildcardProjection" projection.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"a": 1}, {name: kIndexName, wildcardProjection: {a: 1, b: 1}}),
    ErrorCodes.BadValue);
// Cannot create a text index with a "wildcardProjection" projection.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"$**": "text"}, {name: kIndexName, wildcardProjection: {a: 1, b: 1}}),
    ErrorCodes.BadValue);
// Cannot create an wildcard index with a non-object "wildcardProjection" projection.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"a.$**": 1}, {name: kIndexName, wildcardProjection: "string"}),
    ErrorCodes.TypeMismatch);
// Cannot exclude an subfield of _id in an inclusion.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(coll, {"_id.id": 0, a: 1, b: 1, c: 1}),
    ErrorCodes.CannotCreateIndex);
// Cannot include an subfield of _id in an exclusion.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(coll, {"_id.id": 1, a: 0, b: 0, c: 0}),
    ErrorCodes.CannotCreateIndex);

// Cannot specify both a subpath and a projection.
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"a.$**": 1}, {name: kIndexName, wildcardProjection: {a: 1}}),
    ErrorCodes.FailedToParse);
assert.commandFailedWithCode(
    IndexCatalogHelpers.createSingleIndex(
        coll, {"a.$**": 1}, {name: kIndexName, wildcardProjection: {b: 0}}),
    ErrorCodes.FailedToParse);

// Test that you can create a wildcard index on a clustered collection.
const clusteredCollName = "wildcard_clustered";
const clusteredColl = db[clusteredCollName];
assertDropCollection(db, clusteredCollName);
assert.commandWorked(
    db.runCommand({create: clusteredCollName, clusteredIndex: {key: {_id: 1}, unique: true}}));
assert.commandWorked(IndexCatalogHelpers.createSingleIndex(coll, {"$**": 1}, {name: kIndexName}));

// Test that you cannot cluster a collection using a wildcard index.
clusteredColl.drop();
assertDropCollection(db, clusteredCollName);
assert.commandFailedWithCode(
    db.runCommand({create: clusteredCollName, clusteredIndex: {key: {"$**": 1}}}),
    40414);  // Need to specify 'unique'.
assert.commandFailedWithCode(
    db.runCommand({create: clusteredCollName, clusteredIndex: {key: {"$**": 1}, unique: true}}),
    ErrorCodes.InvalidIndexSpecificationOption);
})();
