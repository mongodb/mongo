/**
 * Tests that the index's full specification is included in the oplog entry corresponding to its
 * creation.
 */
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    var rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    var primary = rst.getPrimary();

    var testDB = primary.getDB("test");
    var oplogColl = primary.getDB("local").oplog.rs;

    function testOplogEntryContainsIndexInfoObj(coll, keyPattern, indexOptions) {
        assert.commandWorked(coll.createIndex(keyPattern, indexOptions));
        var allIndexes = coll.getIndexes();
        var indexSpec = GetIndexHelpers.findByKeyPattern(allIndexes, keyPattern);
        assert.neq(
            null,
            indexSpec,
            "Index with key pattern " + tojson(keyPattern) + " not found: " + tojson(allIndexes));

        var indexCreationOplogQuery = {op: "i", ns: testDB.system.indexes.getFullName()};
        var allOplogEntries = oplogColl.find(indexCreationOplogQuery).toArray();
        var found = allOplogEntries.filter(function(entry) {
            return bsonWoCompare(entry.o, indexSpec) === 0;
        });
        assert.eq(1,
                  found.length,
                  "Failed to find full index specification " + tojson(indexSpec) +
                      " in any oplog entry from index creation: " + tojson(allOplogEntries));

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
