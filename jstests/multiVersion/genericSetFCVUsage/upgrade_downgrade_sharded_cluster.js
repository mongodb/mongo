/**
 * Verify that some metadata is properly changed after the upgrade and downgrade of a sharded
 * cluster. More specifically:
 *
 *   1. Create a sharded cluster in replica set running an old binary version
 *   2. Setup some data on cluster
 *   3. Upgrade binaries and FCV of the cluster to the latest version
 *   4. Verify the data consistency after the upgrade procedure
 *   5. Verify that config.settings collection upgrade behavior works correctly
 *   6. Downgrade binaries and FCV of the cluster to an old version
 *   7. Verify the data consistency after the downgrade procedure
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();

function setupClusterAndDatabase(binVersion) {
    const st = new ShardingTest({
        mongos: 1,
        config: 2,
        shards: 2,
        other: {
            mongosOptions: {binVersion: binVersion},
            configOptions: {binVersion: binVersion},
            rsOptions: {
                binVersion: binVersion,
            },
            rs: {nodes: 2},
        },
        // By default, our test infrastructure sets the election timeout to a very high value (24
        // hours). For this test, we need a shorter election timeout because it relies on nodes
        // running an election when they do not detect an active primary. Therefore, we are setting
        // the electionTimeoutMillis to its default value.
        initiateWithDefaultElectionTimeout: true,
    });
    st.configRS.awaitReplication();

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    return st;
}

function getNodeName(node) {
    const info = node.adminCommand({hello: 1});
    return info.setName + "_" + (info.secondary ? "secondary" : "primary");
}

function checkConfigAndShardsFCV(expectedFCV) {
    const configPrimary = st.configRS.getPrimary();

    const shard0Primary = st.rs0.getPrimary();
    const shard0Secondary = st.rs0.getSecondary();
    shard0Secondary.setSecondaryOk();

    const shard1Primary = st.rs1.getPrimary();
    const shard1Secondary = st.rs1.getSecondary();
    shard1Secondary.setSecondaryOk();

    for (const node of [configPrimary, shard0Primary, shard0Secondary, shard1Primary, shard1Secondary]) {
        jsTest.log("Verify that the FCV is properly set on node " + getNodeName(node));

        const fcvDoc = node.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.eq(expectedFCV, fcvDoc.featureCompatibilityVersion.version);
    }
}

function checkConfigSettingsValidatorUpgrade() {
    jsTest.log("Verifying config.settings collection validator upgrade by checking for DOW field");

    const configPrimary = st.configRS.getPrimary();
    const configDB = configPrimary.getDB("config");

    const collectionInfo = configDB.runCommand({listCollections: 1, filter: {name: "settings"}});
    assert.commandWorked(collectionInfo, "Failed to get collection info for config.settings");

    const settingsCollection = collectionInfo.cursor.firstBatch.find((coll) => coll.name === "settings");
    assert(settingsCollection, "config.settings collection not found");

    if (!settingsCollection.options || !settingsCollection.options.validator) {
        jsTest.log("config.settings collection has no validator");
        return;
    }

    const validator = settingsCollection.options.validator;

    const validatorStr = JSON.stringify(validator);
    const hasActiveWindowDOW = validatorStr.includes("activeWindowDOW");

    assert(hasActiveWindowDOW, "Validator does not contain activeWindowDOW field anywhere in the schema");

    jsTest.log("Config.settings validator upgrade verified successfully - activeWindowDOW field found");
}

function checkRangeDeletionMetadataConsistency() {
    if (FeatureFlagUtil.isPresentAndEnabled(st.shard0, "CheckRangeDeletionsWithMissingShardKeyIndex")) {
        return;
    }
    jsTest.log("Executing checkRangeDeletionMetadataConsistency");

    const dbName = "checkMetadataConsistencyTest";

    // Create database for checkMetadataConsistency
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    // Check that the command fails
    assert.commandFailedWithCode(
        st.s.getDB(dbName).runCommand({checkMetadataConsistency: 1, "checkRangeDeletionIndexes": 1}),
        ErrorCodes.InvalidOptions,
    );

    st.s.getDB(dbName).dropDatabase();
}

function checkClusterBeforeUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
}

function checkClusterAfterFCVUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    checkConfigSettingsValidatorUpgrade();
}

function checkClusterAfterBinaryDowngrade(fcv) {
    checkConfigAndShardsFCV(fcv);
}

function checkClusterAfterFCVDowngrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    checkRangeDeletionMetadataConsistency();
}

for (const oldVersion of [lastLTSFCV, lastContinuousFCV]) {
    //////////////////////////////
    // Setting and testing cluster using old binaries in default FCV mode

    jsTest.log("Deploying cluster version " + oldVersion);
    var st = setupClusterAndDatabase(oldVersion);

    checkClusterBeforeUpgrade(oldVersion);

    //////////////////////////////
    // Setting and testing cluster using latest binaries in latest FCV mode

    jsTest.log("Upgrading binaries to latest version");
    st.upgradeCluster("latest");

    jsTest.log("Upgrading FCV to " + latestFCV);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    checkClusterAfterFCVUpgrade(latestFCV);

    //////////////////////////////
    // Setting and testing cluster using old binaries in old FCV mode

    jsTest.log("Downgrading FCV to " + oldVersion);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion, confirm: true}));

    checkClusterAfterFCVDowngrade(oldVersion);

    jsTest.log("Downgrading binaries to version " + oldVersion);
    st.downgradeCluster(oldVersion);

    checkClusterAfterBinaryDowngrade(oldVersion);

    st.stop();
}
