/*
 * Utilities for checking whether SBE is enabled.
 */

load("jstests/libs/discover_topology.js");  // For findNonConfigNodes.
load("jstests/libs/fixture_helpers.js");    // For 'isMongos'

/**
 * Returns whether or not SBE is enabled for the given connection. Assumes that for repl sets and
 * sharded clusters, SBE is either enabled on each node, or disabled on each node.
 * If 'featureFlags' is non-empty, checks if SBE and all the feature flags are enabled.
 * If 'checkAllNodes` is true, explicitly checks if feature flags are enabled for all
 * nodes.
 */
function checkSBEEnabled(theDB, featureFlags = [], checkAllNodes = false) {
    // By default, we find that SBE is enabled. If, for any node, we find that the classic engine is
    // on, `checkResult` will be set to false. This is done intentionally so that in the event that
    // we check all nodes, the effects from previously visited nodes will carry over into the rest.
    // If we are not checking all nodes, `checkResult` is reset to true before each iteration.
    let checkResult = true;

    assert.soon(() => {
        // Some test suites kill the primary, potentially resulting in networking errors. We use:
        //  1. try..catch below to retry the whole check if we failed to discover topology
        //  2. try..catch in the loop to try the next node if the current is killed (if we aren't
        //  checking to ensure that all feature flags are enabled on all nodes).
        let nodes;
        checkResult = true;
        try {
            nodes = DiscoverTopology.findNonConfigNodes(theDB.getMongo());
        } catch (e) {
            return false;
        }

        // Find a non-mongos node and check whether its forceClassicEngine flag is on. We assume
        // either all nodes in the cluster have SBE disabled or none.
        for (const node of nodes) {
            try {
                const conn = new Mongo(node);
                if (FixtureHelpers.isMongos(conn.getDB("admin"))) {
                    continue;
                }

                const getParam = conn.adminCommand({
                    getParameter: 1,
                    internalQueryForceClassicEngine: 1,
                    internalQueryFrameworkControl: 1,
                });

                // v6.0 does not include the new internalQueryFrameworkControl server parameter.
                // Here, we are accounting for both the old and new frameworks (where enabling a
                // certain engine differs semantically).
                if (getParam.hasOwnProperty("internalQueryForceClassicEngine") &&
                    getParam.internalQueryForceClassicEngine) {
                    checkResult = false;
                }

                if (getParam.hasOwnProperty("internalQueryFrameworkControl") &&
                    getParam.internalQueryFrameworkControl === "forceClassicEngine") {
                    checkResult = false;
                }

                if (!getParam.hasOwnProperty("internalQueryForceClassicEngine") &&
                    !getParam.hasOwnProperty("internalQueryFrameworkControl")) {
                    checkResult = false;
                }

                featureFlags.forEach(function(featureFlag) {
                    const featureFlagParam = conn.adminCommand({getParameter: 1, [featureFlag]: 1});
                    checkResult = checkResult && featureFlagParam.hasOwnProperty(featureFlag) &&
                        featureFlagParam[featureFlag]["value"];
                });

                // Exit `assert.soon` if we are only analyzing one node in the cluster.
                if (!checkAllNodes) {
                    return true;
                }
            } catch (e) {
                // Unable to verify that all feature flags are enabled on the current node. Return
                // early from `assert.soon` if we're checking all nodes; otherwise, try the next
                // node.
                if (checkAllNodes) {
                    return false;
                } else {
                    checkResult = true;
                    continue;
                }
            }
        }

        // If we are not checking feature flags on all nodes, this result only occurs when we catch
        // an exception for each node. In this case, the output of `assert.soon` should be false.
        // If we are checking feature flags on all nodes, this result only occurs when we
        // successfully iterate over each node and update the local boolean flags. In this case, the
        // output of `assert.soon` should be true.
        return checkAllNodes;
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
                    internalQueryFrameworkControl: 1,
                    internalQueryForceClassicEngine: 1,
                    featureFlagSbeFull: 1,
                });

                if (getParam.hasOwnProperty("internalQueryFrameworkControl")) {
                    // We say SBE is fully enabled if the engine is on and either
                    // 'featureFlagSbeFull' doesn't exist on the targeted server, or it exists and
                    // is set to true.
                    if (getParam.internalQueryFrameworkControl !== "forceClassicEngine" &&
                        getParam.featureFlagSbeFull.value) {
                        engineMap.sbe++;
                    } else {
                        engineMap.classic++;
                    }
                } else {
                    // 'internalQueryForceClassicEngine' should be set on the previous versions
                    // before 'internalQueryFrameworkControl' is introduced.
                    assert(getParam.hasOwnProperty("internalQueryForceClassicEngine"), getParam);
                    if (!getParam.internalQueryForceClassicEngine.value &&
                        getParam.featureFlagSbeFull.value) {
                        engineMap.sbe++;
                    } else {
                        engineMap.classic++;
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

/**
 * Returns 'true' if SBE is enabled on at least on one node for the given connection 'db'.
 */
function checkSBEEnabledOnSomeNode(db) {
    return checkSBEEnabled(db) || checkBothEnginesAreRunOnCluster(db);
}
