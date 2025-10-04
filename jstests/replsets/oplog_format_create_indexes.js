/**
 * Tests that the index's full specification is included in the oplog entry corresponding to its
 * creation.
 */
import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const testDB = primary.getDB("test");
const oplogColl = primary.getDB("local").oplog.rs;

function testOplogEntryContainsIndexInfoObj(coll, keyPattern, indexOptions) {
    assert.commandWorked(coll.createIndex(keyPattern, indexOptions));
    const allIndexes = coll.getIndexes();
    const indexSpec = IndexCatalogHelpers.findByKeyPattern(allIndexes, keyPattern);

    assert.neq(null, indexSpec, "Index with key pattern " + tojson(keyPattern) + " not found: " + tojson(allIndexes));

    const indexCreationOplogQuery = {
        op: "c",
        ns: testDB.getName() + ".$cmd",
        "o.startIndexBuild": coll.getName(),
    };

    const allOplogEntries = oplogColl.find(indexCreationOplogQuery).toArray();

    // Preserve the JSON version of the originals, as we're going to delete fields.
    const allOplogEntriesJson = tojson(allOplogEntries);
    const indexSpecJson = tojson(indexSpec);

    const found = allOplogEntries.filter((entry) => {
        const entrySpec = entry.o;

        // startIndexes oplog entry contains an array of index specs.
        if (entry.o.indexes) {
            assert.eq(1, entry.o.indexes.length);
            if (indexSpec.name !== entry.o.indexes[0].name) {
                return false;
            }
            // We use assert.docEq() here rather than bsonWoCompare() because we cannot expect the
            // field order in the embedded startIndexBuild index spec to match the original index
            // spec.
            assert.docEq(indexSpec, entry.o.indexes[0]);
            return true;
        }

        delete entrySpec.ns;
        delete entrySpec.createIndexes;
        return bsonWoCompare(indexSpec, entrySpec) === 0;
    });
    assert.eq(
        1,
        found.length,
        "Failed to find full index specification " +
            indexSpecJson +
            " in any oplog entry from index creation: " +
            allOplogEntriesJson,
    );

    assert.commandWorked(coll.dropIndex(keyPattern));
}

// Insert document into collection to avoid optimization for index creation on an empty collection.
assert.commandWorked(testDB.oplog_format.insert({a: 1}));

// Test that options both explicitly included in the command and implicitly filled in with
// defaults by the server are serialized into the corresponding oplog entry.
testOplogEntryContainsIndexInfoObj(testDB.oplog_format, {withoutAnyOptions: 1});
testOplogEntryContainsIndexInfoObj(testDB.oplog_format, {withV1: 1}, {v: 1});
testOplogEntryContainsIndexInfoObj(
    testDB.oplog_format,
    {partialIndex: 1},
    {partialFilterExpression: {field: {$exists: true}}},
);

// Test that the representation of an index's collation in the oplog on a collection with a
// non-simple default collation exactly matches that of the index's full specification.
assert.commandWorked(testDB.runCommand({create: "oplog_format_collation", collation: {locale: "fr"}}));

// Insert document into collection to avoid optimization for index creation on an empty collection.
assert.commandWorked(testDB.oplog_format_collation.insert({a: 1}));

testOplogEntryContainsIndexInfoObj(testDB.oplog_format_collation, {withDefaultCollation: 1});
testOplogEntryContainsIndexInfoObj(
    testDB.oplog_format_collation,
    {withNonDefaultCollation: 1},
    {collation: {locale: "en"}},
);
testOplogEntryContainsIndexInfoObj(testDB.oplog_format_collation, {withV1: 1}, {v: 1});
testOplogEntryContainsIndexInfoObj(
    testDB.oplog_format_collation,
    {withSimpleCollation: 1},
    {collation: {locale: "simple"}},
);

rst.stopSet();
