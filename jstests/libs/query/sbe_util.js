/*
 * Utilities for checking whether SBE is enabled.
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// Constants representing different states of SBE enablement.
export const kSbeFullyEnabled = "sbeFull";
export const kFeatureFlagSbeFullEnabled = "featureFlagSbeFull";
export const kSbeRestricted = "sbeRestricted";
export const kSbeDisabled = "sbeDisabled";

/**
 * Discover nodes in the cluster and call 'checkFunction' on any one mongod node. Returns the value
 * returned by the 'checkFunction' callback.
 */
function discoverNodesAndCheck(theDB, checkFunction) {
    let result = "";
    assert.soon(() => {
        // Some passthrough tests always operate against the primary node of the database primary
        // shard. In such cases, we want to report the SBE mode which matches what the caller will
        // actually see.
        try {
            result = checkFunction(FixtureHelpers.getPrimaryForNodeHostingDatabase(theDB));
            return true;
        } catch (e) {
            // Try non config nodes.
        }
        // If we don't have a primary node for 'theDB' yet, loop over all non-config nodes and check
        // for SBE mode. We need to ensure all nodes have the same SBE mode.
        let nodes;
        try {
            nodes = DiscoverTopology.findNonConfigNodes(theDB.getMongo());
        } catch (e) {
            return false;
        }

        for (const node of nodes) {
            try {
                const conn = new Mongo(node);
                if (FixtureHelpers.isMongos(conn.getDB("admin"))) {
                    continue;
                }
                let mode = checkFunction(conn);
                if (result === "") {
                    result = mode;
                } else if (result !== mode) {
                    // SBE mode not consistent in all nodes, test could fail due to unexpected SBE
                    // mode so quit the test.
                    jsTestLog(
                        "Skipping test because SBE configuration is not consistent across hosts!");
                    quit();
                }

            } catch (e) {
                // Unable to verify on the current node. Try the next node.
                continue;
            }
        }
        return true;
    });
    return result;
}

/**
 * Checks the status of SBE on any one node in the cluster and returns one of the constants
 * kSbeFullyEnabled, kSbeRestricted, kSbeDisabled.
 *
 * Quits test if there is no primary node and we are running in a mixed configuration.
 */
export function checkSbeStatus(theDB) {
    if (theDB !== null) {
        return discoverNodesAndCheck(theDB, (conn) => {
            const getParam = conn.adminCommand({
                getParameter: 1,
                internalQueryFrameworkControl: 1,
            });

            if (!getParam.hasOwnProperty("internalQueryFrameworkControl")) {
                return kSbeDisabled;
            } else if (getParam.internalQueryFrameworkControl === "forceClassicEngine") {
                return kSbeDisabled;
            } else if (FeatureFlagUtil.isPresentAndEnabled(conn, "SbeFull")) {
                return kFeatureFlagSbeFullEnabled;
            } else if (getParam.internalQueryFrameworkControl === "trySbeRestricted") {
                return kSbeRestricted;
            } else if (getParam.internalQueryFrameworkControl === "trySbeEngine") {
                return kSbeFullyEnabled;
            }
            return kSbeDisabled;
        });
    } else {
        // If we don't have a database available, we can only look at the TestData to see what
        // parameters resmoke was given.
        const frameworkControl = TestData.setParameters.internalQueryFrameworkControl
            ? TestData.setParameters.internalQueryFrameworkControl
            : "trySbeRestricted";
        if (frameworkControl == "forceClassicEngine") {
            // Always overrides anything else.
            return kSbeDisabled;
        } else if (TestData.setParameters.featureFlagSbeFull &&
                   TestData.setParameters.featureFlagSbeFull == "true") {
            // Otherwise, if this feature flag is enabled, we ignore the query knob.
            return kFeatureFlagSbeFullEnabled;
        } else if (frameworkControl === "trySbeEngine") {
            return kSbeFullyEnabled;
        } else {
            // If we're here, we must be using 'trySbeRestricted'.
            assert.eq(frameworkControl, "trySbeRestricted");
            return kSbeRestricted;
        }
    }
}

/**
 * Check if featureFlagSbeFull is enabled in the cluster.
 *
 * Quits test if there is no primary node and we are running in a mixed configuration.
 */
export function checkSbeFullFeatureFlagEnabled(theDB) {
    return checkSbeStatus(theDB) === kFeatureFlagSbeFullEnabled;
}

/**
 * Check if SBE is fully enabled in the cluster. This implies that either 'featureFlagSbeFull' is
 * enabled, or the internalQueryFrameworkControl knob is set to 'trySbeEngine'.
 *
 * Quits test if there is no primary node and we are running in a mixed configuration.
 */
export function checkSbeFullyEnabled(theDB) {
    const status = checkSbeStatus(theDB);
    return status === kSbeFullyEnabled || status === kFeatureFlagSbeFullEnabled;
}

/**
 * Check if SBE is either restricted (only select agg stages: $group, $lookup,
 * $_internalUnpackBucket are allowed to be pushed down to sbe) or fully enabled in the cluster.
 *
 * Quits test if there is no primary node and we are running in a mixed configuration.
 */
export function checkSbeRestrictedOrFullyEnabled(theDB) {
    const status = checkSbeStatus(theDB);
    return status === kSbeRestricted || status === kSbeFullyEnabled ||
        status == kFeatureFlagSbeFullEnabled;
}

/**
 * Check if SBE is restricted (only select agg stages: $group, $lookup, $_internalUnpackBucket are
 * allowed to be pushed down to sbe).
 *
 * Quits test if there is no primary node and we are running in a mixed configuration.
 */
export function checkSbeRestricted(theDB) {
    return checkSbeStatus(theDB) === kSbeRestricted;
}

/**
 * Check if we are using the classic engine.
 */
export function checkSbeCompletelyDisabled(theDB) {
    return checkSbeStatus(theDB) === kSbeDisabled;
}
