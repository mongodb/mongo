/**
 * Tests that serverStatus contains the extension.search section with all expected metrics.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    assertOnlyTheseMetricsChanged,
    getMetricsSection,
    setupExtensionMetricsTest,
} from "jstests/noPassthrough/libs/extension_server_status_helpers.js";

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

const kExtensionLib = "libsearch_extension.so";
const kFeatureFlag = "featureFlagSearchExtension";
const kMetricsPath = "extension.search";

checkPlatformCompatibleWithExtensions();

(function ExtensionSearchUsedGetsIncremented() {
    withExtensions(
        {[kExtensionLib]: {}},
        (testConn) => {
            const {coll, testData, getMetrics} = setupExtensionMetricsTest(testConn, kFeatureFlag, kMetricsPath);

            const initialMetrics = getMetrics();
            const result = coll.aggregate([{$search: {}}]).toArray();
            assert.eq(result.length, testData.length, "Search should return all documents");
            const finalMetrics = getMetrics();

            assertOnlyTheseMetricsChanged(initialMetrics, finalMetrics, ["extensionSearchUsed"]);
        },
        ["standalone"],
    );
})();

(function ExtensionSearchMetaUsedGetsIncremented() {
    withExtensions(
        {[kExtensionLib]: {}},
        (testConn) => {
            const {coll, getMetrics} = setupExtensionMetricsTest(testConn, kFeatureFlag, kMetricsPath);

            const initialMetrics = getMetrics();
            coll.aggregate([{$searchMeta: {}}]).toArray();
            const finalMetrics = getMetrics();

            assertOnlyTheseMetricsChanged(initialMetrics, finalMetrics, ["extensionSearchUsed"]);
        },
        ["standalone"],
    );
})();

(function ExtensionSearchUsedNotIncrementedForNonSearchStages() {
    withExtensions(
        {[kExtensionLib]: {}},
        (testConn) => {
            const {coll, getMetrics} = setupExtensionMetricsTest(testConn, kFeatureFlag, kMetricsPath);

            const initialMetrics = getMetrics();
            coll.aggregate([{$match: {a: 1}}]).toArray();
            const finalMetrics = getMetrics();

            assertOnlyTheseMetricsChanged(initialMetrics, finalMetrics, []);
        },
        ["standalone"],
    );
})();

(function ExplainIncrementsExtensionSearchUsed() {
    withExtensions(
        {[kExtensionLib]: {}},
        (testConn) => {
            const {coll, getMetrics} = setupExtensionMetricsTest(testConn, kFeatureFlag, kMetricsPath);

            const initialMetrics = getMetrics();
            coll.explain().aggregate([{$search: {}}]);
            const finalMetrics = getMetrics();

            assertOnlyTheseMetricsChanged(initialMetrics, finalMetrics, ["extensionSearchUsed"]);
        },
        ["standalone"],
    );
})();

(function VectorSearchDoesNotIncrementSearchCounters() {
    withExtensions(
        {[kExtensionLib]: {}, "libvector_search_extension.so": {}},
        (testConn) => {
            const {coll, getMetrics} = setupExtensionMetricsTest(testConn, kFeatureFlag, kMetricsPath);
            assert.commandWorked(
                testConn.getDB("admin").runCommand({setParameter: 1, featureFlagVectorSearchExtension: true}),
            );

            const initialMetrics = getMetrics();
            coll.aggregate([{$vectorSearch: {}}]).toArray();
            const finalMetrics = getMetrics();

            assertOnlyTheseMetricsChanged(initialMetrics, finalMetrics, []);
        },
        ["standalone"],
    );
})();

(function ShardedExtensionSearchIncrementsOnShardPrimary() {
    withExtensions(
        {[kExtensionLib]: {}},
        (testConn, shardingTest) => {
            const {coll, getMetrics} = setupExtensionMetricsTest(testConn, kFeatureFlag, kMetricsPath);

            const shardPrimary = shardingTest.rs0.getPrimary();
            const initialShardMetrics = getMetricsSection(shardPrimary, kMetricsPath);
            const initialMongosMetrics = getMetrics();

            coll.aggregate([{$search: {}}]).toArray();

            const finalShardMetrics = getMetricsSection(shardPrimary, kMetricsPath);
            assertOnlyTheseMetricsChanged(initialShardMetrics, finalShardMetrics, ["extensionSearchUsed"]);
            // Mongos does not execute stages, so its counters should not change.
            const finalMongosMetrics = getMetrics();
            assertOnlyTheseMetricsChanged(initialMongosMetrics, finalMongosMetrics, []);
        },
        ["sharded"],
    );
})();

(function ShardedExplainIncrementsOnShardPrimary() {
    withExtensions(
        {[kExtensionLib]: {}},
        (testConn, shardingTest) => {
            const {coll, getMetrics} = setupExtensionMetricsTest(testConn, kFeatureFlag, kMetricsPath);

            const shardPrimary = shardingTest.rs0.getPrimary();
            const initialShardMetrics = getMetricsSection(shardPrimary, kMetricsPath);
            const initialMongosMetrics = getMetrics();

            coll.explain().aggregate([{$search: {}}]);

            const finalShardMetrics = getMetricsSection(shardPrimary, kMetricsPath);
            assertOnlyTheseMetricsChanged(initialShardMetrics, finalShardMetrics, ["extensionSearchUsed"]);
            // Mongos does not execute stages, so its counters should not change.
            const finalMongosMetrics = getMetrics();
            assertOnlyTheseMetricsChanged(initialMongosMetrics, finalMongosMetrics, []);
        },
        ["sharded"],
    );
})();
