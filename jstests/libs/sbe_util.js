/*
 * Utilities for checking whether SBE is enabled.
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * Returns whether or not SBE is enabled for the given connection. Assumes that for repl sets and
 * sharded clusters, SBE is either enabled on each node, or disabled on each node.
 * If 'featureFlags' is non-empty, checks if SBE and all the feature flags are enabled.
 * If 'checkAllNodes` is true, explicitly checks if feature flags are enabled for all
 * nodes.
 */
export function checkSBEEnabled(theDB, featureFlags = [], checkAllNodes = false) {
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
                    internalQueryFrameworkControl: 1,
                });

                if (getParam.hasOwnProperty("internalQueryFrameworkControl") &&
                    getParam.internalQueryFrameworkControl === "forceClassicEngine") {
                    checkResult = false;
                }

                if (!getParam.hasOwnProperty("internalQueryFrameworkControl")) {
                    checkResult = false;
                }

                featureFlags.forEach(function(featureFlag) {
                    const kFeatureFlagPrefix = "featureFlag";
                    if (featureFlag.startsWith(kFeatureFlagPrefix)) {
                        featureFlag = featureFlag.substring(kFeatureFlagPrefix.length);
                    }
                    checkResult =
                        checkResult && FeatureFlagUtil.isPresentAndEnabled(conn, featureFlag);
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
