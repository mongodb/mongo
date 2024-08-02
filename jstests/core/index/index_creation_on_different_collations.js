/**
 * This test ensures that creating an index on a collection with non-simple collation always
 * respects the collation option used. This is required for sharding since the sharding index has to
 * use the simple collation regardless of the collection's default collation.
 *
 * @tags: [
 *     # We do not expect there to be an existing collection as it can conflict with the
 *     # createCollection call with a "collection already exists with different options" error.
 *     assumes_no_implicit_collection_creation_after_drop
 * ]
 */

import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";

const coll = db.index_creation_on_different_collations;
coll.drop();

const nonSimpleCollationOptions = {
    locale: "en_US",
    strength: 2,
    caseLevel: false
};

assert.commandWorked(db.createCollection(coll.getName(), {collation: nonSimpleCollationOptions}));

// Create an index that doesn't specify the collation option. This should use the default value.
assert.commandWorked(
    db.runCommand({createIndexes: coll.getName(), indexes: [{key: {x: 1}, name: "x_1"}]}));

assert.commandWorked(db.runCommand({
    createIndexes: coll.getName(),
    indexes: [{key: {x: 1}, name: "x_1_1", collation: {locale: 'simple'}}]
}));

const indexesFound = coll.getIndexes();
assert.neq(null,
           IndexCatalogHelpers.findByKeyPattern(indexesFound, {x: 1}, {locale: "simple"}),
           "Failed to find simple collation index for {x: 1} / Found: " + tojson(indexesFound));
assert.neq(null,
           IndexCatalogHelpers.findByKeyPattern(indexesFound, {x: 1}, {
               locale: "en_US",
               caseLevel: false,
               caseFirst: "off",
               strength: 2,
               numericOrdering: false,
               alternate: "non-ignorable",
               maxVariable: "punct",
               normalization: false,
               backwards: false,
               version: "57.1"
           }),
           "Failed to find complex collation index for {x: 1} / Found: " + tojson(indexesFound));
