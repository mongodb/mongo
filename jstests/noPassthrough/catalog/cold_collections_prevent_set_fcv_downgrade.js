/**
 * Verifies that attempting to downgrade the Feature Compatibility Version (FCV) fails with a CannotDowngrade error
 * when cold collections are present and the 'CreateSupportsStorageTierOptions' feature flag is disabled in the target FCV.
 *
 * This test runs against both a sharded cluster and a replica set:
 * 1. Starts the cluster
 * 2. Creates a cold collection
 * 3. Attempts to downgrade FCV to 'last-lts'
 * 4. Validates that the FCV downgrade fails with CannotDowngrade error
 *
 * TODO (SERVER-122670) Remove this test once the feature flag is removed.
 */

import "jstests/multiVersion/libs/verify_versions.js";

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {PersistenceProviderUtil} from "jstests/libs/server-rss/persistence_provider_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test will be skipped when:
//  1. The cluster is not a Disaggregated Storage cluster, which is determined by checking if the
// persistence provider supports cold collections.
//  2. The feature flag 'CreateSupportsStorageTierOptions' is enabled at the downgrade FCV, which
// means cold collections are supported and downgrade should succeed.
function shouldSkipTest(testDB) {
    const downgradeFCV = binVersionToFCV("last-lts");
    const flagDoc = FeatureFlagUtil.getFeatureFlagDoc(testDB, "CreateSupportsStorageTierOptions");
    if (flagDoc && flagDoc.version && MongoRunner.compareBinVersions(downgradeFCV, flagDoc.version) >= 0) {
        jsTest.log.info(
            "Skipping test because CreateSupportsStorageTierOptions is enabled at last-lts FCV " + downgradeFCV,
        );
        return true;
    }

    const storageTierSettable = PersistenceProviderUtil.allNodesHavePropertyWithValue(
        testDB,
        "supportsColdCollections",
        true,
    );

    if (!storageTierSettable) {
        jsTest.log.info("Skipping test because the cluster does not support cold collections");
        return true;
    }

    return false;
}

function testDowngradeBlockedWithColdCollection(testDB) {
    const collName = "cold_coll";

    const adminDB = testDB.getSiblingDB("admin");

    // Verify we're on latest FCV.
    checkFCV(adminDB, latestFCV);

    const downgradeFCV = binVersionToFCV("last-lts");

    // Create a cold collection.
    assert.commandWorked(testDB.createCollection(collName, {storageTier: {collection: "cold"}}));

    // Attempt to downgrade FCV - this should fail due to cold collection.
    const fcvResult = adminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true});
    assert.commandFailedWithCode(
        fcvResult,
        ErrorCodes.CannotDowngrade,
        "FCV downgrade should have failed due to cold collection",
    );

    // Verify the downgrade failed with a CannotDowngrade error and the error message mentions cold
    // storage tier.
    assert(
        fcvResult.errmsg.includes("cold storage tier"),
        `Error message should mention cold storage tier, got: ${tojson(fcvResult)}`,
    );

    // Verify FCV is still at latest.
    checkFCV(adminDB, latestFCV);
}

// Test with a replica set.
{
    jsTest.log.info("--- Testing with ReplSetTest ---");
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    if (shouldSkipTest(primary)) {
        rst.stopSet();
        quit();
    }

    const testDB = primary.getDB(jsTestName());
    testDowngradeBlockedWithColdCollection(testDB);
    rst.stopSet();
}

// Test with a sharded cluster.
{
    jsTest.log.info("--- Testing with ShardingTest ---");
    const st = new ShardingTest({shards: 1, mongos: 1});

    const testDB = st.s.getDB(jsTestName());
    testDowngradeBlockedWithColdCollection(testDB);
    st.stop();
}
