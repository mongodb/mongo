/**
 * Tests parsing and validation of compression options for column store indexes.
 * @tags: [
 *   # The $collStats stage is not supported inside a transaction.
 *   does_not_support_transactions,
 *   requires_collstats,
 *
 *   # Column store indexes are still under a feature flag and require full SBE.
 *   uses_column_store_index,
 *   featureFlagColumnstoreIndexes,
 *   featureFlagSbeFull,
 *
 *   # In passthrough suites, this test makes direct connections to mongod instances that compose
 *   # the passthrough fixture in order to perform additional validation. Tenant migration,
 *   # alternate read concern values, and step downs can cause these connections to fail.
 *   tenant_migration_incompatible,
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/columnstore_util.js");       // For setUpServerForColumnStoreIndexTest.
load("jstests/libs/discover_topology.js");      // For findNonConfigNodes
load("jstests/libs/fixture_helpers.js");        // For isMongos
load("jstests/libs/index_catalog_helpers.js");  // For IndexCatalogHelpers
load("jstests/libs/sbe_util.js");               // For checkSBEEnabled.

const columnstoreEnabled = checkSBEEnabled(
    db, ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"], true /* checkAllNodes */);
if (!columnstoreEnabled) {
    jsTestLog("Skipping columnstore index test since the feature flag is not enabled.");
    return;
}

if (!setUpServerForColumnStoreIndexTest(db)) {
    return;
}

const coll = db.column_store_index_compression;
coll.drop();

/**
 * The IndexStatsReader connects to each mongod in a fixture (standalone, replica set, or sharded
 * cluster) so that we can verify index properties that we want to hold for all data-bearing nodes.
 */
class IndexStatsReader {
    constructor() {
        this.connections = {};
    }

    static openConnectionToMongod(node) {
        const conn = new Mongo(node);
        conn.setSecondaryOk();
        return !FixtureHelpers.isMongos(conn.getDB("admin")) ? conn : false;
    }

    /**
     * Poll the list of indexes from the given collection in 'remoteDB' until it contains a desired
     * index _and_ that index is ready. Ready indexes do not have a "buildUUID" field.
     */
    static assertIndexReadySoon(remoteDB, collectionName, indexName) {
        assert.soon(() => {
            const indexResult = assert.commandWorked(
                remoteDB.runCommand({listIndexes: collectionName, includeBuildUUIDs: true}));
            return indexResult.cursor.firstBatch.findIndex(
                       indexSpec =>
                           (indexSpec.name === indexName && !("buildUUID" in indexSpec))) >= 0;
        });
    }

    * statsForEachMongod(collection, indexName) {
        let nonConfigNodes;
        assert.soon(() => {
            nonConfigNodes = DiscoverTopology.findNonConfigNodes(collection.getDB().getMongo());
            return nonConfigNodes.length > 0;
        });

        for (let node of nonConfigNodes) {
            const conn = this.connections[node] ||
                (this.connections[node] = IndexStatsReader.openConnectionToMongod(node));
            if (conn) {
                const remoteDB = conn.getDB(collection.getDB().getName());

                // Wait until the index is done building and all its data is synced to disk.
                IndexStatsReader.assertIndexReadySoon(remoteDB, collection.getName(), indexName);
                assert.commandWorked(db.adminCommand({fsync: 1}));

                const collStats = remoteDB[collection.getName()]
                                      .aggregate([{$collStats: {storageStats: {}}}])
                                      .next();
                yield {node, indexDetails: collStats.storageStats.indexDetails[indexName]};
            }
        }
    }
}
const reader = new IndexStatsReader();

function parseCompressorFromCreationString(creationString) {
    const [[_, compressorName], ...additionalMatches] =
        [...creationString.matchAll(/(?:^|,)block_compressor=(\w*)/g)];

    // There should not be more than one block_compressor value defined in the creation string.
    assert.eq(additionalMatches.length, 0, creationString);

    return compressorName;
}

// Test that specifying a fictional compression module fails.
assert.commandFailedWithCode(IndexCatalogHelpers.createSingleIndex(
                                 coll,
                                 {"$**": "columnstore"},
                                 {name: "three_comma_index", columnstoreCompressor: "middleout"}),
                             ErrorCodes.InvalidIndexSpecificationOption);

// Test that the column store indexes are created with Zstandard block compression by default.
const defaultIndex = "index_with_default_compression";
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": "columnstore"}, {name: defaultIndex});
for (let {node, indexDetails} of reader.statsForEachMongod(coll, defaultIndex)) {
    assert.eq(parseCompressorFromCreationString(indexDetails.creationString),
              "zstd",
              {node, indexDetails});
}

// Test creation of a column store with Zlib block compression.
const zlibIndex = "index_with_zlib_compression";
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": "columnstore"}, {name: zlibIndex, columnstoreCompressor: "zlib"});
for (let {node, indexDetails} of reader.statsForEachMongod(coll, zlibIndex)) {
    assert.eq(parseCompressorFromCreationString(indexDetails.creationString),
              "zlib",
              {node, indexDetails});
}

// Test creation of a column store with Snappy block compression.
const snappyIndex = "index_with_snappy_compression";
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": "columnstore"}, {name: snappyIndex, columnstoreCompressor: "snappy"});
for (let {node, indexDetails} of reader.statsForEachMongod(coll, snappyIndex)) {
    assert.eq(parseCompressorFromCreationString(indexDetails.creationString),
              "snappy",
              {node, indexDetails});
}

// Add some documents so we can test that index builds succeed both with and without compression.
const kNumDocuments = 32;
const approxUncompressedSize = kNumDocuments * 1024 * 1024;
for (let i = 0; i < kNumDocuments; i++) {
    // Insert a ~1MB document.
    assert.commandWorked(
        coll.insert({_id: i, payload: i.toString().padStart(2, "0").repeat(1024 * 1024 / 2)}));
}

// Test creation of a column store with block compression explicitly disabled.
const rawIndex = "index_without_compression";
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": "columnstore"}, {name: rawIndex, columnstoreCompressor: "none"});
for (let {node, indexDetails} of reader.statsForEachMongod(coll, rawIndex)) {
    assert.eq(parseCompressorFromCreationString(indexDetails.creationString),
              "none",
              {node, indexDetails});
}

// Test creation of a column store with Zstandard block compression explicitly specified.
const zstdIndex = "index_with_zstd_compression";
IndexCatalogHelpers.createIndexAndVerifyWithDrop(
    coll, {"$**": "columnstore"}, {name: zstdIndex, columnstoreCompressor: "zstd"});
for (let {node, indexDetails} of reader.statsForEachMongod(coll, zstdIndex)) {
    assert.eq(parseCompressorFromCreationString(indexDetails.creationString),
              "zstd",
              {node, indexDetails});
}
}());
