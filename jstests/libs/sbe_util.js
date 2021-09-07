/*
 * Utilities for checking whether SBE is enabled.
 */

load("jstests/libs/discover_topology.js");  // For findNonConfigNodes.
load("jstests/libs/fixture_helpers.js");    // For 'isMongos'

/**
 * Returns whether or not SBE is enabled for the given connection. Assumes that for repl sets and
 * sharded clusters, SBE is either enabled on each node, or disabled on each node.
 * If 'featureFlags' is non-empty, checks if SBE and all the feature flags are enabled.
 */
function checkSBEEnabled(theDB, featureFlags = []) {
    let checkResult = true;

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

        // Find a non-mongos node and check whether its forceClassicEngine flag is on. We
        // assume either all nodes in the cluster have SBE disabled or none.
        for (const node of nodes) {
            try {
                const conn = new Mongo(node);
                if (FixtureHelpers.isMongos(conn.getDB("admin"))) {
                    continue;
                }

                featureFlags.forEach(function(featureFlag) {
                    const featureFlagParam = conn.adminCommand({getParameter: 1, [featureFlag]: 1});
                    checkResult = checkResult && featureFlagParam.hasOwnProperty(featureFlag) &&
                        featureFlagParam[featureFlag]["value"];
                });

                const getParam =
                    conn.adminCommand({getParameter: 1, internalQueryForceClassicEngine: 1});
                if (getParam.hasOwnProperty("internalQueryForceClassicEngine") &&
                    getParam.internalQueryForceClassicEngine) {
                    checkResult = false;
                }
                return true;
            } catch (e) {
                continue;
            }
        }

        return false;
    });

    return checkResult;
}
