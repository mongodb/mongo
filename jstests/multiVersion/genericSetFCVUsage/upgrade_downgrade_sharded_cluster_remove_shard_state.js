/**
 * upgrade_downgrade_sharded_cluster_range_deletion.js is taken as the base for this test.
 * Verify that attribute state from config.shards is properly removed after the upgrade.
 * Also verifies that the attribute is not re-added during the downgrade. More specifically:
 *
 * TODO SERVER-116437: Remove once 9.0 becomes last lts.
 *
 *   1. Create a sharded cluster in replica set running an old binary version
 *   2. Verify that the state attribute is present on config.shards
 *   3. Upgrade binaries and FCV of the cluster to the latest version
 *   4. Verify that the state attribute is removed from config.shards
 *   5. Downgrade binaries and FCV of the cluster to an old version
 *   6. Verify that the state attribute is still removed from config.shards
 *
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconnect} from "jstests/replsets/rslib.js";

const dbName = jsTestName();

const kShardsNs = "config.shards";

function validateNumberOfDocumentsWithStateAttributeInShardsCollection(st, expectedNumberOfDocuments) {
    assert.soon(() => {
        try {
            // we search for elements in the shards collection with the state field still set
            let documents = st.config0
                .getCollection(kShardsNs)
                .find({"state": {$exists: true}})
                .toArray();
            // we will usually expect either 0 or 2 documents in the result; if the result doesn't
            // match the expectation, we show the result as a json
            assert.eq(
                expectedNumberOfDocuments,
                documents.length,
                "Expected " +
                    expectedNumberOfDocuments +
                    " documents with state field still set in " +
                    kShardsNs +
                    " collection, found: " +
                    tojson(documents),
            );
            return true;
        } catch (e) {
            if (isNetworkError(e)) {
                // It's a network error, attempting to reconnect and retry.
                reconnect(st.shard0);
                return false;
            }
            // It's a different error, re-throw it immediately.
            // assert.soon will catch it and fail the assertion.
            throw e;
        }
    });
}

function setupClusterAndDatabase(binVersion) {
    const st = new ShardingTest({
        mongos: 1,
        config: 2,
        shards: 2,
        other: {
            mongosOptions: {binVersion: binVersion},
            configOptions: {binVersion: binVersion},
            rsOptions: {binVersion: binVersion},
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
        jsTest.log.info("Verify that the FCV is properly set on node " + getNodeName(node));

        const fcvDoc = node.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.eq(expectedFCV, fcvDoc.featureCompatibilityVersion.version);
    }
}

function checkClusterBeforeUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    // before the FCV upgrade, we expect exactly 2 documents to have the state attribute
    validateNumberOfDocumentsWithStateAttributeInShardsCollection(st, 2);
}

function checkClusterAfterFCVUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    // after the FCV upgrade, we should have 0 documents with the state attribute
    validateNumberOfDocumentsWithStateAttributeInShardsCollection(st, 0);
}

function checkClusterAfterBinaryDowngrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    // after the FCV upgrade, we don't expect to restore the state attribute
    validateNumberOfDocumentsWithStateAttributeInShardsCollection(st, 0);
}

if (lastLTSFCV != "8.0") {
    print("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

for (const oldVersion of [lastLTSFCV, lastContinuousFCV]) {
    //////////////////////////////
    // Setting and testing cluster using old binaries in default FCV mode

    jsTest.log.info("Deploying cluster version " + oldVersion);
    var st = setupClusterAndDatabase(oldVersion);

    checkClusterBeforeUpgrade(oldVersion);

    //////////////////////////////
    // Setting and testing cluster using latest binaries in latest FCV mode

    jsTest.log.info("Upgrading binaries to latest version");
    st.upgradeCluster("latest");

    jsTest.log.info("Upgrading FCV to " + latestFCV);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    checkClusterAfterFCVUpgrade(latestFCV);

    //////////////////////////////
    // Setting and testing cluster using old binaries in old FCV mode

    jsTest.log.info("Downgrading FCV to " + oldVersion);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion, confirm: true}));

    jsTest.log.info("Downgrading binaries to version " + oldVersion);
    st.downgradeCluster(oldVersion);

    checkClusterAfterBinaryDowngrade(oldVersion);

    st.stop();
}
