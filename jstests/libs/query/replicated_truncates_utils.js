/**
 * Helpers for replicated truncates.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {PersistenceProviderUtil} from "jstests/libs/server-rss/persistence_provider_util.js";

/**
 * Determines whether the persistence provider requires replicated truncates.
 *
 * This function checks if all nodes in the replica set have the "shouldUseReplicatedTruncates"
 * property set to true.
 *
 * @param {Object} conn - The connection object to the MongoDB instance or replica set.
 * @param {Array} nodes - Optional array of specific nodes to check. If null,
 *                        all nodes in the replica set will be checked.
 * @returns {boolean} - True if all nodes have shouldUseReplicatedTruncates set to true,
 *                      false otherwise.
 */
export function persistenceProviderRequiresReplicatedTruncates(conn, nodes) {
    return PersistenceProviderUtil.allNodesHavePropertyWithValue(conn, "shouldUseReplicatedTruncates", true, nodes);
}

/**
 * Determines whether the feature flag mandates replicated truncates for deletions.
 *
 * This function checks if the "UseReplicatedTruncatesForDeletions" feature flag is present
 * and enabled on the provided connection.
 *
 * @param {Object} conn - The database connection object used to query feature flag status.
 * @returns {boolean} - True if the feature flag is present and enabled, false otherwise.
 */
export function featureFlagMandatesReplicatedTruncates(conn) {
    return FeatureFlagUtil.isPresentAndEnabled(conn, "UseReplicatedTruncatesForDeletions");
}

/**
 * Determines whether the system uses replicated truncates for deleting pre-images.
 *
 * There are two modes for deleting pre-images from the "system.preimages" collection:
 * 1. Independent local deletion on each node - can cause different nodes in the replica set
 *    to have different views of the pre-images collection state.
 * 2. Replicated truncates - only the primary executes deletions via "truncateRange()" calls,
 *    and deletions are replicated via the oplog. All nodes are expected to have the same
 *    view of the pre-images collection.
 *
 * The function first checks the persistence provider property for replicated truncates support.
 * If not enabled there, it checks the "featureFlagUseReplicatedTruncatesForDeletions" feature flag.
 *
 * @param {Object} conn - The database connection object used to query the replica set configuration.
 * @param {Array} nodes - Optional list of nodes to check for the persistence provider property.
 *                        If not provided, all nodes of the current fixture will be checked.
 * @returns {boolean} True if the system uses replicated truncates, false otherwise.
 */
export function systemUsesReplicatedTruncates(conn, nodes = null) {
    // First check the persistence provider property. If this has replicated truncates enabled, we
    // go with it. If the persistence provider does not have replicated truncates enabled, check the
    // current value of the feature flag.
    return (
        persistenceProviderRequiresReplicatedTruncates(conn, nodes) ||
        featureFlagMandatesReplicatedTruncates(conn, nodes)
    );
}
