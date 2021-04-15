/*
 * Utilities for checking whether SBE is enabled.
 */

load("jstests/libs/discover_topology.js");  // For findNonConfigNodes.
load("jstests/libs/fixture_helpers.js");    // For 'isMongos'

function checkSBEEnabled(theDB) {
    const nodes = DiscoverTopology.findNonConfigNodes(theDB.getMongo());

    for (const node of nodes) {
        // Find a non-mongos node and check whether its SBE feature flag is on. We assume either
        // all nodes in the cluster have SBE on or none.
        const conn = new Mongo(nodes[0]);
        if (FixtureHelpers.isMongos(conn.getDB("admin"))) {
            continue;
        }

        const getParam = conn.adminCommand({getParameter: 1, featureFlagSBE: 1});
        return getParam.hasOwnProperty("featureFlagSBE") && getParam.featureFlagSBE.value;
    }
    assert(false, "Couldn't find mongod");
}
