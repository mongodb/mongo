/**
 * Tests that the index's full specification is included in the oplog entry corresponding to its
 * creation.
 */
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();

    const testDB = primary.getDB("test");
    const oplogColl = primary.getDB("local").oplog.rs;

    function testOplogEntryContainsIndexInfoObj(coll, keyPattern, indexOptions) {
        assert.commandWorked(coll.createIndex(keyPattern, indexOptions));
        const allIndexes = coll.getIndexes();
        const indexSpec = GetIndexHelpers.findByKeyPattern(allIndexes, keyPattern);

        assert.neq(
            null,
            indexSpec,
            "Index with key pattern " + tojson(keyPattern) + " not found: " + tojson(allIndexes));

        // Find either the old-style insert into system.indexes index creations, or new-style
        // createIndexes command entries.
        const indexCreationOplogQuery = {
            $or: [
                {op: "i", ns: testDB.system.indexes.getFullName()},
                {op: "c", ns: testDB.getName() + ".$cmd", "o.createIndexes": coll.getName()}
            ]
        };

        const allOplogEntries = oplogColl.find(indexCreationOplogQuery).toArray();

        // Preserve the JSON version of the originals, as we're going to delete fields.
        const allOplogEntriesJson = tojson(allOplogEntries);
        const indexSpecJson = tojson(indexSpec);

        // Because of differences between the new and old oplog entries for createIndexes,
        // treat the namespace part separately and compare entries without ns field.
        const indexSpecNs = indexSpec.ns;
        delete indexSpec.ns;
        const found = allOplogEntries.filter((entry) => {
            const entryNs = entry.o.ns || testDB.getName() + "." + entry.o.createIndexes;
            const entrySpec = entry.o;
            delete entrySpec.ns;
            delete entrySpec.createIndexes;
            return indexSpecNs === entryNs && bsonWoCompare(indexSpec, entrySpec) === 0;
        });
        assert.eq(1,
                  found.length,
                  "Failed to find full index specification " + indexSpecJson +
                      " in any oplog entry from index creation: " + allOplogEntriesJson);

        assert.commandWorked(coll.dropIndex(keyPattern));
    }

    // Test that options both explicitly included in the command and implicitly filled in with
    // defaults by the server are serialized into the corresponding oplog entry.
    testOplogEntryContainsIndexInfoObj(testDB.oplog_format, {withoutAnyOptions: 1});
    testOplogEntryContainsIndexInfoObj(testDB.oplog_format, {withV1: 1}, {v: 1});
    testOplogEntryContainsIndexInfoObj(testDB.oplog_format,
                                       {partialIndex: 1},
                                       {partialFilterExpression: {field: {$exists: true}}});

    // Test that the representation of an index's collation in the oplog on a collection with a
    // non-simple default collation exactly matches that of the index's full specification.
    assert.commandWorked(
        testDB.runCommand({create: "oplog_format_collation", collation: {locale: "fr"}}));
    testOplogEntryContainsIndexInfoObj(testDB.oplog_format_collation, {withDefaultCollation: 1});
    testOplogEntryContainsIndexInfoObj(
        testDB.oplog_format_collation, {withNonDefaultCollation: 1}, {collation: {locale: "en"}});
    testOplogEntryContainsIndexInfoObj(testDB.oplog_format_collation, {withV1: 1}, {v: 1});
    testOplogEntryContainsIndexInfoObj(
        testDB.oplog_format_collation, {withSimpleCollation: 1}, {collation: {locale: "simple"}});

    rst.stopSet();
})();
