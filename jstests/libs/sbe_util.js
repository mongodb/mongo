/*
 * Utilities for checking whether SBE is enabled.
 */

load("jstests/libs/discover_topology.js");  // For findNonConfigNodes.
load("jstests/libs/fixture_helpers.js");    // For 'isMongos'

function checkSBEEnabled(theDB) {
    let checkResult = false;

    assert.soon(() => {
        // Some test suites kill the primary, potentially resulting in networking errors. We use:
        //  1. try..catch below to retry the whole check if we failed to discover topology
        //  2. try..catch in the loop to try the next node if the current is killed
        let nodes;
        try {
            nodes = DiscoverTopology.findNonConfigNodes(theDB.getMongo());
        } catch (e) {
            return false;
        }

        // Find a non-mongos node and check whether its SBE feature flag is on. We assume either all
        // nodes in the cluster have SBE on or none.
        for (const node of nodes) {
            try {
                const conn = new Mongo(node);
                if (FixtureHelpers.isMongos(conn.getDB("admin"))) {
                    continue;
                }

                const getParam = conn.adminCommand({getParameter: 1, featureFlagSBE: 1});
                checkResult =
                    getParam.hasOwnProperty("featureFlagSBE") && getParam.featureFlagSBE.value;
                return true;
            } catch (e) {
                continue;
            }
        }

        return false;
    });

    return checkResult;
}
