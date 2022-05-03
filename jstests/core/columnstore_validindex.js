/**
 * Tests parsing and validation of columnstore indexes.
 * @tags: [
 *   # Uses index building in background.
 *   requires_background_index,
 *   # Columnstore indexes are new in 6.1.
 *   requires_fcv_61,
 *   # TODO SERVER-66021 replication uses the bulk builder which isn't implemented yet.
 *   assumes_standalone_mongod,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/index_catalog_helpers.js");  // For "IndexCatalogHelpers."

const getParamResponse =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagColumnstoreIndexes: 1}));
const columnstoreEnabled = getParamResponse.hasOwnProperty("featureFlagColumnstoreIndexes") &&
    getParamResponse.featureFlagColumnstoreIndexes.value;
if (!columnstoreEnabled) {
    jsTestLog("Skipping test about validating columnstore index specifications since the" +
              " columnstore index feature flag is not enabled.");
    return;
}

const kCollectionName = "columnstore_validindex";
const coll = db.getCollection(kCollectionName);

const kIndexName = "columnstore_index";

// Can create a valid columnstore index.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(coll, {"$**": "columnstore"}, {name: kIndexName});

// Can create a columnstore index with foreground & background construction.
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": "columnstore"}, {background: false, name: kIndexName});
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": "columnstore"}, {background: true, name: kIndexName});

// TODO SERVER-64365 Can create a columnstore index with index level collation.
assert.commandFailedWithCode(
    coll.createIndex({"$**": "columnstore"}, {collation: {locale: "fr"}, name: kIndexName}),
    ErrorCodes.CannotCreateIndex);

// TODO SERVER-63123 Can create a valid columnstore index with subpaths.
assert.commandFailedWithCode(coll.createIndex({"a.$**": "columnstore"}),
                             ErrorCodes.CannotCreateIndex);

// Cannot create a columnstore index with any of the following options which could apply to other
// indexes.
function assertCannotUseOptions(options) {
    assert.commandFailedWithCode(coll.createIndex({"$**": "columnstore"}, options),
                                 ErrorCodes.CannotCreateIndex);
}
coll.dropIndexes();
assertCannotUseOptions({partialFilterExpression: {a: {"$gt": 0}}});
assertCannotUseOptions({sparse: true});
assertCannotUseOptions({expireAfterSeconds: 3000});
assertCannotUseOptions({v: 0});
assertCannotUseOptions({v: 1});
assertCannotUseOptions({unique: true});

// TODO SERVER-63123 Can create a columnstore index with an inclusion projection.
// IndexCatalogHelpers.createIndexAndVerifyWithDrop(
// coll, {"$**": "columnstore"}, {columnstoreProjection: {a: 1, b: 1, c: 1}, name: kIndexName});
assert.commandFailedWithCode(
    coll.createIndex({"$**": "columnstore"},
                     {columnstoreProjection: {a: 1, b: 1, c: 1}, name: kIndexName}),
    ErrorCodes.InvalidIndexSpecificationOption);
// TODO SERVER-63123 Can create a columnstore index with an exclusion projection.
// IndexCatalogHelpers.createIndexAndVerifyWithDrop(
// coll, {"$**": "columnstore"}, {columnstoreProjection: {a: 0, b: 0, c: 0}, name: kIndexName});
assert.commandFailedWithCode(
    coll.createIndex({"$**": "columnstore"},
                     {columnstoreProjection: {a: 0, b: 0, c: 0}, name: kIndexName}),
    ErrorCodes.InvalidIndexSpecificationOption);
// TODO SERVER-63123 Can include _id in an exclusion.
// IndexCatalogHelpers.createIndexAndVerifyWithDrop(
//     coll,
//     {"$**": "columnstore"},
//     {columnstoreProjection: {_id: 1, a: 0, b: 0, c: 0}, name: kIndexName});
// TODO SERVER-63123 Can exclude _id in an exclusion.
// IndexCatalogHelpers.createIndexAndVerifyWithDrop(
//     coll,
//     {"$**": "columnstore"},
//     {columnstoreProjection: {_id: 0, a: 1, b: 1, c: 1}, name: kIndexName});

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

// TODO SERVER-63123 Cannot create an columnstore index with mixed inclusion exclusion.
// assert.commandFailedWithCode(
//     IndexCatalogHelpers.createSingleIndex(
//         coll, {"$**": "columnstore"}, {name: kIndexName, columnstoreProjection: {a: 1, b: 0}}),
//     31254);

// TODO SERVER-63123 Cannot create an columnstore index with computed fields.
// assert.commandFailedWithCode(IndexCatalogHelpers.createSingleIndex(
//                                  coll,
//                                  {"$**": "columnstore"},
//                                  {name: kIndexName, columnstoreProjection: {a: 1, b: "string"}}),
//                              51271);

// TODO SERVER-63123 Cannot create an columnstore index with an empty projection.
// assert.commandFailedWithCode(
//     IndexCatalogHelpers.createSingleIndex(
//         coll, {"$**": "columnstore"}, {name: kIndexName, columnstoreProjection: {}}),
//     ErrorCodes.FailedToParse);

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

// TODO SERVER-63123 Cannot create an columnstore index with a non-object "columnstoreProjection"
// projection. assert.commandFailedWithCode(
//     IndexCatalogHelpers.createSingleIndex(
//         coll, {"a.$**": "columnstore"}, {name: kIndexName, columnstoreProjection: "string"}),
//     ErrorCodes.TypeMismatch);

// TODO SERVER-63123 Cannot exclude an subfield of _id in an inclusion.
// assert.commandFailedWithCode(
//     IndexCatalogHelpers.createSingleIndex(coll, {"_id.id": 0, a: 1, b: 1, c: 1}),
//     ErrorCodes.CannotCreateIndex);

// TODO SERVER-63123 Cannot include an subfield of _id in an exclusion.
// assert.commandFailedWithCode(
//     IndexCatalogHelpers.createSingleIndex(coll, {"_id.id": 1, a: 0, b: 0, c: 0}),
//     ErrorCodes.CannotCreateIndex);

// TODO SERVER-63123 Cannot specify both a subpath and a projection.
// assert.commandFailedWithCode(
//     IndexCatalogHelpers.createSingleIndex(
//         coll, {"a.$**": "columnstore"}, {name: kIndexName, columnstoreProjection: {a: 1}}),
//     ErrorCodes.FailedToParse);
// assert.commandFailedWithCode(
//     IndexCatalogHelpers.createSingleIndex(
//         coll, {"a.$**": "columnstore"}, {name: kIndexName, columnstoreProjection: {b: 0}}),
//     ErrorCodes.FailedToParse);
})();
