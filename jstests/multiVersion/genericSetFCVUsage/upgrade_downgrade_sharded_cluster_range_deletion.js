/**
 * upgrade_downgrade_sharded_cluster.js is taken as the base for this test.
 * Verify that RangeDeletion tasks are properly changed after the upgrade and downgrade of a sharded
 * cluster. More specifically:
 *
 * TODO SERVER-103046: Remove once 9.0 becomes last lts.
 *
 *   1. Create a sharded cluster in replica set running an old binary version
 *   2. Setup some data on cluster. Inserts 10 documents and starts the moveChunk
 *   3. Upgrade binaries and FCV of the cluster to the latest version
 *   4. Verify the data consistency after the upgrade procedure and config.rangeDeletions
 *   5. Downgrade binaries and FCV of the cluster to an old version
 *   6. Verify the data consistency and config.rangeDeletions after the downgrade procedure
 *
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconnect} from "jstests/replsets/rslib.js";

const dbName = jsTestName();

const kRangeDeletionNs = "config.rangeDeletions";
const testRangeDeletionNS = dbName + '.testRangeDeletion';
const numOrphanedDocs = 10;

/**
 * The feature flag TerminateSecondaryReadsUponRangeDeletion was enabled by default starting in
 * version 8.2. The associated query termination functionality was also introduced in version 8.2.
 * However, the preMigrationShardVersion field within the RangeDeletionTask began being populated
 * earlier, in version 8.1. The introduction of this field in 8.1 did not coincide with a change in
 * functionality at that time (specifically, the query termination feature was not yet available).
 * Regarding the population of the preMigrationShardVersion field:
 * If a collection is newly created in version 8.1, the RangeDeletionTask associated with it will
 * contain the populated preMigrationShardVersion field. Conversely, if a collection was originally
 * created in version 8.0 (and subsequently upgraded) or if it was downgraded from version 8.2
 * to 8.1, the RangeDeletionTask will not contain a populated preMigrationShardVersion field.
 */
function createTestCollection(st) {
    TestData.skipCheckOrphans = true;
    assert.commandWorked(st.s.adminCommand({shardCollection: testRangeDeletionNS, key: {_id: 1}}));
    var batch = st.s.getCollection(testRangeDeletionNS).initializeOrderedBulkOp();
    for (var i = 0; i < numOrphanedDocs; i++) {
        batch.insert({_id: i});
    }
    assert.commandWorked(batch.execute());
    assert.commandWorked(st.splitAt(testRangeDeletionNS, {_id: 0}));
    st.s.adminCommand({moveChunk: testRangeDeletionNS, find: {_id: 0}, to: st.shard1.shardName});
    assert.eq(1, st.shard0.getCollection(kRangeDeletionNs).find().toArray().length);
}

function validateRangeDeletionTasks(st) {
    var terminateSecondaryFeatureFlagEnabled = FeatureFlagUtil.isPresentAndEnabled(
        st.configRS.getPrimary().getDB('admin'), "TerminateSecondaryReadsUponRangeDeletion");
    // preMigrationShardVersion field is not removed during downgrade.
    // Therefore, we only need to check that it is present when the relevant Feature Flag is
    // enabled.
    if (terminateSecondaryFeatureFlagEnabled) {
        assert.soon(() => {
            try {
                var doc =
                    st.shard0.getCollection(kRangeDeletionNs).findOne({nss: testRangeDeletionNS});
                assert(doc.hasOwnProperty('preMigrationShardVersion'));
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
}

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
                setParameter: {disableResumableRangeDeleter: true},
            },
            rs: {nodes: 2},
        },
        // By default, our test infrastructure sets the election timeout to a very high value (24
        // hours). For this test, we need a shorter election timeout because it relies on nodes
        // running an election when they do not detect an active primary. Therefore, we are setting
        // the electionTimeoutMillis to its default value.
        initiateWithDefaultElectionTimeout: true
    });
    st.configRS.awaitReplication();

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    createTestCollection(st);

    return st;
}

function getNodeName(node) {
    const info = node.adminCommand({hello: 1});
    return info.setName + '_' + (info.secondary ? 'secondary' : 'primary');
}

function checkConfigAndShardsFCV(expectedFCV) {
    const configPrimary = st.configRS.getPrimary();

    const shard0Primary = st.rs0.getPrimary();
    const shard0Secondary = st.rs0.getSecondary();
    shard0Secondary.setSecondaryOk();

    const shard1Primary = st.rs1.getPrimary();
    const shard1Secondary = st.rs1.getSecondary();
    shard1Secondary.setSecondaryOk();

    for (const node
             of [configPrimary, shard0Primary, shard0Secondary, shard1Primary, shard1Secondary]) {
        jsTest.log('Verify that the FCV is properly set on node ' + getNodeName(node));

        const fcvDoc = node.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.eq(expectedFCV, fcvDoc.featureCompatibilityVersion.version);
    }
}

function checkClusterBeforeUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
}

function checkClusterAfterFCVUpgrade(fcv) {
    checkConfigAndShardsFCV(fcv);
    validateRangeDeletionTasks(st);
}

function checkClusterAfterBinaryDowngrade(fcv) {
    checkConfigAndShardsFCV(fcv);
}

for (const oldVersion of [lastLTSFCV, lastContinuousFCV]) {
    //////////////////////////////
    // Setting and testing cluster using old binaries in default FCV mode

    jsTest.log('Deploying cluster version ' + oldVersion);
    var st = setupClusterAndDatabase(oldVersion);

    checkClusterBeforeUpgrade(oldVersion);

    //////////////////////////////
    // Setting and testing cluster using latest binaries in latest FCV mode

    jsTest.log('Upgrading binaries to latest version');
    st.upgradeCluster('latest');

    jsTest.log('Upgrading FCV to ' + latestFCV);
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    checkClusterAfterFCVUpgrade(latestFCV);

    //////////////////////////////
    // Setting and testing cluster using old binaries in old FCV mode

    jsTest.log('Downgrading FCV to ' + oldVersion);
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion, confirm: true}));

    jsTest.log('Downgrading binaries to version ' + oldVersion);
    st.downgradeCluster(oldVersion);

    checkClusterAfterBinaryDowngrade(oldVersion);

    st.stop();
}
