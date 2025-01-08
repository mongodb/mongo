// FCV4.4 is required for creating a collection with a long name.
// @tags: [
//   requires_capped,
//   # TODO SERVER-73967: Remove this tag.
//   does_not_support_stepdowns,
// ]

// Tests for the "create" command.
import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";

function is73orBelow(db) {
    const res = db.getSiblingDB("admin")
                    .system.version.find({_id: "featureCompatibilityVersion"})
                    .toArray();
    return MongoRunner.compareBinVersions(res[0].version, "8.0") < 0;
}

// "create" command rejects invalid options.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(db.createCollection("create_collection", {unknown: 1}),
                             ErrorCodes.IDLUnknownField);

// Cannot create a collection with null characters.
assert.commandFailedWithCode(db.createCollection("\0ab"), ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(db.createCollection("a\0b"), ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(db.createCollection("ab\0"), ErrorCodes.InvalidNamespace);

// The collection name length limit was upped in 4.4, try creating a collection with a longer
// name than previously allowed.
const collLength = 250;
const longCollName = 'a'.repeat(collLength);
assert.commandWorked(db.runCommand({drop: longCollName}));
assert.commandWorked(db.createCollection(longCollName));

// The collection name for internal db collections is longer then 255 but still capped to 512.
if (!FixtureHelpers.isMongos(db) && !TestData.testingReplicaSetEndpoint) {
    const internalCollLength = is73orBelow(db) ? 245 : 500;
    const internalLongCollName = 'a'.repeat(internalCollLength);
    assert.commandWorked(db.runCommand({drop: internalLongCollName}));
    assert.commandWorked(db.getSiblingDB("config").runCommand({drop: internalLongCollName}));
    assert.commandWorked(db.getSiblingDB("config").createCollection(internalLongCollName));
    assert.commandWorked(db.getSiblingDB("admin").runCommand({drop: internalLongCollName}));
    assert.commandWorked(db.getSiblingDB("admin").createCollection(internalLongCollName));
}

// Cannot create a collection with a nss above the limit (255 including the db name).
const longCollNameInvalid = 'a'.repeat(255);
assert.commandWorked(db.runCommand({drop: longCollNameInvalid}));
assert.commandFailedWithCode(db.createCollection(longCollNameInvalid),
                             [ErrorCodes.InvalidNamespace]);
//
// Tests for "idIndex" field.
//

// "idIndex" field not allowed with "viewOn".
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandWorked(db.createCollection("create_collection"));
assert.commandFailedWithCode(db.runCommand({
    create: "create_view",
    viewOn: "create_collection",
    idIndex: {key: {_id: 1}, name: "_id_"}
}),
                             ErrorCodes.InvalidOptions);

// "idIndex" field not allowed with "autoIndexId".
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(
    db.createCollection("create_collection",
                        {autoIndexId: false, idIndex: {key: {_id: 1}, name: "_id_"}}),
    ErrorCodes.InvalidOptions);

// "idIndex" field must be an object.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(db.createCollection("create_collection", {idIndex: 1}),
                             ErrorCodes.TypeMismatch);

// "idIndex" field cannot be empty.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(db.createCollection("create_collection", {idIndex: {}}),
                             ErrorCodes.FailedToParse);

// "idIndex" field must be a specification for an _id index.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(
    db.createCollection("create_collection", {idIndex: {key: {a: 1}, name: "a_1"}}),
    ErrorCodes.BadValue);

// "idIndex" field must have "key" equal to {_id: 1}.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(
    db.createCollection("create_collection", {idIndex: {key: {a: 1}, name: "_id_"}}),
    ErrorCodes.BadValue);

// The name of an _id index gets corrected to "_id_".
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandWorked(
    db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "a_1"}}));
var indexSpec = IndexCatalogHelpers.findByKeyPattern(db.create_collection.getIndexes(), {_id: 1});
assert.neq(indexSpec, null);
assert.eq(indexSpec.name, "_id_", tojson(indexSpec));

// "idIndex" field must only contain fields that are allowed for an _id index.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(
    db.createCollection("create_collection",
                        {idIndex: {key: {_id: 1}, name: "_id_", sparse: true}}),
    ErrorCodes.InvalidIndexSpecificationOption);

// "create" creates v=2 _id index when "v" is not specified in "idIndex".
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandWorked(
    db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "_id_"}}));
indexSpec = IndexCatalogHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.v, 2, tojson(indexSpec));

// "create" creates v=1 _id index when "idIndex" has "v" equal to 1.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandWorked(
    db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
indexSpec = IndexCatalogHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.v, 1, tojson(indexSpec));

// "create" creates v=2 _id index when "idIndex" has "v" equal to 2.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandWorked(
    db.createCollection("create_collection", {idIndex: {key: {_id: 1}, name: "_id_", v: 2}}));
indexSpec = IndexCatalogHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.v, 2, tojson(indexSpec));

// "collation" field of "idIndex" must match collection default collation.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(
    db.createCollection("create_collection",
                        {idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "en_US"}}}),
    ErrorCodes.BadValue);

assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(db.createCollection("create_collection", {
    collation: {locale: "fr_CA"},
    idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "en_US"}}
}),
                             ErrorCodes.BadValue);

assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandFailedWithCode(db.createCollection("create_collection", {
    collation: {locale: "fr_CA"},
    idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "simple"}}
}),
                             ErrorCodes.BadValue);

assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandWorked(db.createCollection("create_collection", {
    collation: {locale: "en_US", strength: 3},
    idIndex: {key: {_id: 1}, name: "_id_", collation: {locale: "en_US"}}
}));
indexSpec = IndexCatalogHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.collation.locale, "en_US", tojson(indexSpec));

// If "collation" field is not present in "idIndex", _id index inherits collection default
// collation.
assert.commandWorked(db.runCommand({drop: "create_collection"}));
assert.commandWorked(db.createCollection(
    "create_collection", {collation: {locale: "en_US"}, idIndex: {key: {_id: 1}, name: "_id_"}}));
indexSpec = IndexCatalogHelpers.findByName(db.create_collection.getIndexes(), "_id_");
assert.neq(indexSpec, null);
assert.eq(indexSpec.collation.locale, "en_US", tojson(indexSpec));

//
// Tests the combination of the "capped", "size" and "max" fields in createCollection().
//

// When "capped" is true, the "size" field needs to be present.
assert.commandFailedWithCode(db.createCollection('capped_no_size_no_max', {capped: true}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.createCollection('capped_no_size', {capped: true, max: 10}),
                             ErrorCodes.InvalidOptions);
assert.commandWorked(db.runCommand({drop: "no_capped"}));
assert.commandWorked(db.createCollection('no_capped'), {capped: false});
assert.commandWorked(db.runCommand({drop: "capped_no_max"}));
assert.commandWorked(db.createCollection('capped_no_max', {capped: true, size: 256}));
assert.commandWorked(db.runCommand({drop: "capped_with_max_and_size"}));
assert.commandWorked(
    db.createCollection('capped_with_max_and_size', {capped: true, max: 10, size: 256}));

// When the "size" field is present, "capped" needs to be true.
assert.commandFailedWithCode(db.createCollection('size_no_capped', {size: 256}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.createCollection('size_capped_false', {capped: false, size: 256}),
                             ErrorCodes.InvalidOptions);

// The remainder of this test file will not work if all collections are automatically clustered
// because a repeat attempt to create a collection will not have ``clusteredIndex`` set but
// the existing collection will.
if (ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo())) {
    quit();
}

assert.commandWorked(db.runCommand({drop: "create_collection"}));

// Creating a collection that already exists with no options specified reports success.
assert.commandWorked(db.createCollection("create_collection"));
assert.commandWorked(db.createCollection("create_collection"));

assert.commandWorked(db.runCommand({drop: "create_collection"}));

// Creating a collection that already exists with the same options reports success.
assert.commandWorked(db.createCollection("create_collection"), {collation: {locale: "fr"}});
assert.commandWorked(db.createCollection("create_collection"), {collation: {locale: "fr"}});

// Creating a collection that already exists with different options reports failure.
assert.commandFailedWithCode(db.createCollection("create_collection", {collation: {locale: "uk"}}),
                             ErrorCodes.NamespaceExists);
