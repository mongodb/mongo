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

        // Find a non-mongos node and check whether its forceClassicEngine flag is on. We
        // assume either all nodes in the cluster have SBE disabled or none.
        for (const node of nodes) {
            try {
                const conn = new Mongo(node);
                if (FixtureHelpers.isMongos(conn.getDB("admin"))) {
                    continue;
                }

                const getParam = conn.adminCommand({
                    getParameter: 1,
                    internalQueryForceClassicEngine: 1,
                    internalQueryEnableSlotBasedExecutionEngine: 1
                });
                if (getParam.hasOwnProperty("internalQueryForceClassicEngine") &&
                    !getParam.internalQueryForceClassicEngine) {
                    checkResult = true;
                }
                // Some versions use a different parameter to enable SBE instead of disabling it.
                if (getParam.hasOwnProperty("internalQueryEnableSlotBasedExecutionEngine") &&
                    getParam.internalQueryEnableSlotBasedExecutionEngine) {
                    checkResult = true;
                }

                featureFlags.forEach(function(featureFlag) {
                    const featureFlagParam = conn.adminCommand({getParameter: 1, [featureFlag]: 1});
                    checkResult = checkResult && featureFlagParam.hasOwnProperty(featureFlag) &&
                        featureFlagParam[featureFlag]["value"];
                });

                return true;
            } catch (e) {
                continue;
            }
        }

        return false;
    });

    return checkResult;
}

/**
 * If 'theDB' corresponds to a node in a cluster, then returns true if the cluster that it
 * belongs to has at least one node that has SBE enabled and at least one node that has it
 * disabled; false otherwise.
 */
function checkBothEnginesAreRunOnCluster(theDB) {
    let result = false;
    assert.soon(() => {
        if (!FixtureHelpers.isMongos(theDB) && !FixtureHelpers.isReplSet(theDB)) {
            return true;
        }

        // Retry the check if we fail to discover the topology (this can happen if the test
        // suite has killed the primary).
        let nodes;
        try {
            nodes = DiscoverTopology.findNonConfigNodes(theDB.getMongo());
        } catch (e) {
            return false;
        }

        let engineMap = {sbe: 0, classic: 0};

        for (const node of nodes) {
            // If we cannot contact a node because it was killed or is otherwise unreachable, we
            // skip it and check the other nodes in the cluster. For our purposes, this is ok
            // because test suites which step down/kill certain nodes are configured to use
            // exactly one engine, whereas the test suites which are configured use both engines
            // (namely, the multiversion suites), do not step down/kill nodes.
            try {
                const conn = new Mongo(node);
                if (FixtureHelpers.isMongos(conn.getDB("admin"))) {
                    continue;
                }

                const getParam = conn.adminCommand({
                    getParameter: 1,
                    internalQueryForceClassicEngine: 1,
                    internalQueryEnableSlotBasedExecutionEngine: 1
                });

                if (getParam.hasOwnProperty("internalQueryForceClassicEngine")) {
                    if (getParam.internalQueryForceClassicEngine) {
                        engineMap.classic++;
                    } else {
                        engineMap.sbe++;
                    }
                }

                // Some versions use a different parameter to enable SBE instead of disabling it.
                if (getParam.hasOwnProperty("internalQueryEnableSlotBasedExecutionEngine")) {
                    if (!getParam.internalQueryEnableSlotBasedExecutionEngine) {
                        engineMap.classic++;
                    } else {
                        engineMap.sbe++;
                    }
                }

                result = (engineMap.sbe > 0 && engineMap.classic > 0);
                if (result) {
                    return true;
                }
            } catch (e) {
                continue;
            }
        }

        return true;
    });

    return result;
}
