/**
 * Tests that serverStatus contains the extension.search section with all expected metrics.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod failed to start");

const adminDB = conn.getDB("admin");

(function SearchServerStatusMetricsAppearByDefault() {
    // Get serverStatus and verify the extension.search section exists.
    const serverStatus = adminDB.runCommand({serverStatus: 1});
    assert.commandWorked(serverStatus);

    // Verify the extension.search section exists (section name with dot appears as top-level field).
    assert(serverStatus.hasOwnProperty("metrics"));
    assert(serverStatus.metrics.hasOwnProperty("extension"), serverStatus.metrics);
    assert(serverStatus.metrics.extension.hasOwnProperty("search"), serverStatus.metrics.extension);
    assert(serverStatus.metrics.extension.search != null, serverStatus.metrics.extension);

    const searchSection = serverStatus.metrics.extension.search;
    assert(searchSection.hasOwnProperty("legacySearchUsed"), searchSection);
    assert(searchSection.hasOwnProperty("extensionSearchUsed"), searchSection);
    assert(searchSection.hasOwnProperty("onViewKickbackRetries"), searchSection);
    assert(searchSection.hasOwnProperty("inUnionWithKickbackRetries"), searchSection);
    assert(searchSection.hasOwnProperty("inLookupKickbackRetries"), searchSection);
    assert(searchSection.hasOwnProperty("inHybridSearchKickbackRetries"), searchSection);
})();

(function SearchServerStatusMetricsCanBeRequested() {
    const explicitServerStatus = adminDB.runCommand({serverStatus: 1, "metrics.extension.search": 1});
    assert.commandWorked(explicitServerStatus);

    // Make sure the chain metrics -> extension -> search exists.
    assert(explicitServerStatus.hasOwnProperty("metrics"));
    assert(explicitServerStatus.metrics.hasOwnProperty("extension"));
    assert(explicitServerStatus.metrics.extension.hasOwnProperty("search"));
})();

MongoRunner.stopMongod(conn);

// TODO SERVER-122716: Add tests to verify extensionSearchUsed and legacySearchUsed are incremented
// as expected. This can be modeled on extension_vector_search_server_status.js.
// Also add additional tests for each of the following:
//   - onViewKickbackRetries (TODO SERVER-122432)
//   - inUnionWithKickbackRetries (TODO SERVER-122430)
//   - inLookupKickbackRetries (TODO SERVER-122433)
//   - inHybridSearchKickbackRetries (TODO SERVER-122434)
