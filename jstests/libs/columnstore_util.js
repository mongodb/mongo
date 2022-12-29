/*
 * Utilities for column store indexes.
 */

load("jstests/libs/discover_topology.js");  // For findNonConfigNodes.
// For areAllCollectionsClustered.
load("jstests/libs/clustered_collections/clustered_collection_util.js");
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/sbe_util.js");                                // For checkSBEEnabled.

/**
 * Checks if the test is eligible to run and sets the appropriate parameters to use column store
 * indexes. Returns true if set up was successful.
 */
function setUpServerForColumnStoreIndexTest(db) {
    if (!checkSBEEnabled(db)) {
        jsTestLog("Skipping columnstore index test since SBE is disabled");
        return false;
    }

    let nodes = DiscoverTopology.findNonConfigNodes(db.getMongo());

    for (const node of nodes) {
        const conn = new Mongo(node);
        if (FixtureHelpers.isMongos(conn.getDB("admin"))) {
            continue;
        }

        const createColumnIndexParameter =
            getParameter(conn, "failpoint.createColumnIndexOnAllCollections");
        if (createColumnIndexParameter.mode) {
            // Test is already configured to create columnstore indexes on all collections, skip
            // it so that we don't create double indexes.
            jsTestLog(
                "Skipping columnstore index test since column indexes are already on all collections.");
            return false;
        }

        if (ClusteredCollectionUtil.areAllCollectionsClustered(conn)) {
            // Columnstore indexes are incompatible with clustered collections.
            jsTestLog("Skipping columnstore index test since all collections are clustered.");
            return false;
        }

        break;
    }

    setParameterOnAllHosts(nodes, "internalQueryColumnScanMinAvgDocSizeBytes", 0);
    setParameterOnAllHosts(nodes, "internalQueryColumnScanMinCollectionSizeBytes", 0);
    setParameterOnAllHosts(nodes, "internalQueryColumnScanMinNumColumnFilters", 0);

    return true;
}
