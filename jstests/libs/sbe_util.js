/*
 * Utilities for checking whether SBE is enabled.
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// Constants representing different states of SBE enablement.
const kSbeFullyEnabled = "sbeFull";
const kSbeRestricted = "sbeRestricted";
const kSbeDisabled = "sbeDisabled";

/**
 * Discover nodes in the cluster and call 'checkFunction' on any one mongod node. Returns the value
 * returned by the 'checkFunction' callback.
 */
function discoverNodesAndCheck(theDB, checkFunction) {
    let result;
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
                result = checkFunction(conn);
                return true;

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
 * For tryBonsai*, we return kSbeFullyEnabled when featureFlagSbeFull is on and kSbeRestricted when
 * featureFlagSbeFull is off. This reflects the expected SBE behavior when we cannot use Bonsai and
 * instead fall back to SBE without Bonsai.
 */
function checkSbeStatus(theDB) {
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
            return kSbeFullyEnabled;
        } else if (getParam.internalQueryFrameworkControl === "trySbeRestricted" ||
                   getParam.internalQueryFrameworkControl === "tryBonsai" ||
                   getParam.internalQueryFrameworkControl === "tryBonsaiExperimental") {
            return kSbeRestricted;
        } else if (getParam.internalQueryFrameworkControl === "trySbeEngine") {
            return kSbeFullyEnabled;
        }
        return kSbeDisabled;
    });
}

/**
 * Check if SBE is fully enabled in the cluster.
 */
export function checkSbeFullyEnabled(theDB) {
    return checkSbeStatus(theDB) === kSbeFullyEnabled;
}

/**
 * Check if SBE is either restricted (only select agg stages: $group, $lookup,
 * $_internalUnpackBucket are allowed to be pushed down to sbe) or fully enabled in the cluster.
 */
export function checkSbeRestrictedOrFullyEnabled(theDB) {
    const status = checkSbeStatus(theDB);
    return status === kSbeRestricted || status === kSbeFullyEnabled;
}

/**
 * Check if SBE is restricted (only select agg stages: $group, $lookup, $_internalUnpackBucket are
 * allowed to be pushed down to sbe).
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
