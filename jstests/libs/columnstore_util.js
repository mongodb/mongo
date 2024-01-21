/*
 * Utilities for column store indexes.
 */

import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

/**
 * Updates server parameters to disable column scan query planning heuristics so that column scan
 * will always be considered.
 *
 * This is intentionally done in all column scan correctness tests because we want to exercise
 * column scan semantics regardless of whether column scan is performant on the test data. Coverage
 * for the planning heuristics behavior is included in unit tests, no passthrough tests, and perf
 * tests.
 */
export function fullyEnableColumnScan(nodes) {
    // Since the CSI query planning heuristics are OR-ed together, we can set any one of
    // [internalQueryColumnScanMinAvgDocSizeBytes, internalQueryColumnScanMinCollectionSizeBytes,
    // internalQueryColumnScanMinNumColumnFilters] to zero in order to fully enable column scan.
    setParameterOnAllHosts(nodes, "internalQueryColumnScanMinNumColumnFilters", 0);
}

/**
 * Returns true if the current testing environment is one where column store index creation is
 * expected to succeed. Otherwise, logs the reason why the test will not create column store indexes
 * and returns false.
 */
export function safeToCreateColumnStoreIndex(db) {
    return safeToCreateColumnStoreIndexInCluster(
        DiscoverTopology.findNonConfigNodes(db.getMongo()));
}

export function safeToCreateColumnStoreIndexInCluster(nodes) {
    for (const node of nodes) {
        const conn = new Mongo(node);
        if (FixtureHelpers.isMongos(conn.getDB("admin"))) {
            continue;
        }

        const fpName = "failpoint.createColumnIndexOnAllCollections";
        const getParamRes = conn.getDB("admin").runCommand({getParameter: 1, [fpName]: 1});
        if (!getParamRes.ok) {
            return false;
        }

        if (getParamRes[fpName].mode) {
            // Test is already configured to create column store indexes on all collections; skip
            // it so that we don't create double indexes.
            jsTestLog("Note: declining to create column store index, because they are already " +
                      "on all collections.");
            return false;
        }

        if (ClusteredCollectionUtil.areAllCollectionsClustered(conn)) {
            // Columnstore indexes are incompatible with clustered collections.
            jsTestLog("Note: declining to create column store index, because all collections " +
                      "are clustered.");
            return false;
        }

        if (!FeatureFlagUtil.isEnabled(conn, "ColumnstoreIndexes")) {
            jsTestLog("Note: declining to create column store index, because " +
                      "featureFlagColumnstoreIndexes is disabled");
            return false;
        }
    }

    return true;
}

/**
 * Checks if the test is eligible to run and sets the appropriate parameters to use column store
 * indexes. Returns true if setup was successful.
 */
export function setUpServerForColumnStoreIndexTest(db) {
    if (!checkSbeFullyEnabled(db)) {
        jsTestLog("Skipping column store index test since SBE is disabled");
        return false;
    }

    let nodes = DiscoverTopology.findNonConfigNodes(db.getMongo());
    if (!safeToCreateColumnStoreIndexInCluster(nodes)) {
        jsTestLog(
            "Skipping column store index test in suite where column store index creation may fail");
        return false;
    }

    // TODO SERVER-75026: Re-enable CSI in parallel tests.
    // Note that we should not fully enable columnscans during the parallel tests due to the side
    // effect of clearing the SBE plan cache. Fully enabling column scans should only be done
    // in non-parallel environments.
    if ((TestData || {}).isParallelTest) {
        return false;
    } else {
        fullyEnableColumnScan(nodes);
    }

    return true;
}
