// Tests behaviour of the preimage remover where initially the primary and secondary are on
// different binary versions, and the behaviour after upgrading to latest.
//
// @tags: [
//  featureFlagUseUnreplicatedTruncatesForDeletions,
//  requires_replication,
// ]
import "jstests/multiVersion/libs/multi_rs.js";
import {getPreImagesCollection} from "jstests/libs/change_stream_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    setupTimeBasedPreImageRetentionPolicyTest,
    verifyPreImages
} from "jstests/noPassthrough/libs/change_stream_pre_image_time_based_expiration_utils.js";

function serverParametersForVersionString(version) {
    let options = {
        binVersion: version,
    };
    // Reduce period of removal job to speed up test.
    let serverParams = {expiredChangeStreamPreImageRemovalJobSleepSecs: 1};
    if (version == "latest") {
        serverParams["preImagesCollectionTruncateMarkersMinBytes"] = 1;
    }
    options["setParameter"] = serverParams;
    return options;
}

function setupMixedVersionReplicaSetTest(binVersionList) {
    const nodeOptions = binVersionList.map(serverParametersForVersionString);

    const rst = new ReplSetTest({nodes: nodeOptions});
    rst.startSet();
    // Allow test cases to have complete control over which node is primary.
    rst.initiateWithHighElectionTimeout();

    return rst;
}

function setChangeStreamPreImageRemoverCurrentTimeAndWaitForAllNodes(
    rst, currentTimeForTimeBasedExpiration) {
    rst.nodes.forEach(node => {
        const currentTimeFailPoint = configureFailPoint(
            node,
            "changeStreamPreImageRemoverCurrentTime",
            {currentTimeForTimeBasedExpiration: currentTimeForTimeBasedExpiration});

        // Wait until at least 1 complete cycle of pre-image removal job is completed.
        currentTimeFailPoint.wait({timesEntered: 2});
    });
}

function verifyPreImagesForAllNodes(rst, expectedPreImagesForNodes, collectionsInfo) {
    // Verify that at some point in time, all expired pre-images will be deleted.
    rst.nodes.forEach((node, idx) => {
        const preImageColl = getPreImagesCollection(node);
        assert.soon(
            () => {
                return preImageColl.find().toArray().length ==
                    expectedPreImagesForNodes[idx].length;
            },
            () => {
                const preImages = preImageColl.find().toArray();
                return `Host (${node.host}): expected ${
                    tojson(expectedPreImagesForNodes[idx])} for collectionsInfo ${
                    tojson(collectionsInfo)} but found ${tojson(preImages)}`;
            });
        verifyPreImages(preImageColl, expectedPreImagesForNodes[idx], collectionsInfo);

        if (node.host === rst.getPrimary().host) {
            // Await for primary to replicate deletes, in case it does not use unreplicated
            // truncates.
            rst.awaitReplication();
        }
    });
}

function setExpireAfterSeconds(rst, expireAfterSeconds) {
    // Enable time-based pre-image expiration and configure the 'expireAfterSeconds'.
    assert.commandWorked(rst.getPrimary().getDB("admin").runCommand({
        setClusterParameter:
            {changeStreamOptions: {preAndPostImages: {expireAfterSeconds: expireAfterSeconds}}}
    }));
}

// Test case: primary using truncate deletes, secondary with replicated deletes.
// The secondary is expected to not delete anything because the primary is not replicating deletes.
// The situation should be resolved on upgrading the node.
function testPrimaryLatestSecondariesLastLts(shouldTruncateAllAfterUpgrade) {
    jsTestLog("Running testPrimaryLatestSecondariesLastLts. shouldTruncateAllAfterUpgrade: " +
              shouldTruncateAllAfterUpgrade);

    const rst = setupMixedVersionReplicaSetTest(["latest", "last-lts"]);
    const primary = rst.getPrimary();
    const expireAfterSeconds = shouldTruncateAllAfterUpgrade ? 1 : 3600;
    const {currentTimeForTimeBasedExpiration, _unused, shouldRetainDocs, allDocs, collectionsInfo} =
        setupTimeBasedPreImageRetentionPolicyTest(primary, primary, expireAfterSeconds);

    // Fix wall time used by pre image remover.
    setChangeStreamPreImageRemoverCurrentTimeAndWaitForAllNodes(rst,
                                                                currentTimeForTimeBasedExpiration);

    // Verify that when time-based pre-image expiration disabled, no pre-images are not deleted.
    verifyPreImagesForAllNodes(rst, [allDocs, allDocs], collectionsInfo);

    // Enable time based expiration.
    setExpireAfterSeconds(rst, expireAfterSeconds);

    // The secondary is expected to not have removed any documents.
    verifyPreImagesForAllNodes(rst, [shouldRetainDocs, allDocs], collectionsInfo);

    // After upgrading the secondary to a version with truncate deletes, the preImages should be
    // deleted.
    rst.upgradeSecondaries(serverParametersForVersionString("latest"));

    // With shouldTruncateAllAfterUpgrade there's no need to simulate the current time given
    // the low value for expireAfterSeconds (and that we want all documents to be truncated).
    if (!shouldTruncateAllAfterUpgrade) {
        // On the other hand, if we expect some documents to survive, we need to re-enable the
        // failpoint to simulate the time to force expiration.
        setChangeStreamPreImageRemoverCurrentTimeAndWaitForAllNodes(
            rst, currentTimeForTimeBasedExpiration);
    }
    const upgradedNodeExpectedDocs = shouldTruncateAllAfterUpgrade ? [] : shouldRetainDocs;
    verifyPreImagesForAllNodes(rst, [shouldRetainDocs, upgradedNodeExpectedDocs], collectionsInfo);

    rst.stopSet();
}

// Test case: primary using replicated deletes, secondary using truncate deletes.
// The secondary might try to delete the same document both with truncate and by applying the
// replicated delete. This should be fine.
function testPrimaryLastLtsSecondariesLatest(shouldTruncateAllAfterUpgrade) {
    jsTestLog("Running testPrimaryLastLtsSecondariesLatest. shouldTruncateAllAfterUpgrade: " +
              shouldTruncateAllAfterUpgrade);
    const rst = setupMixedVersionReplicaSetTest(["last-lts", "latest"]);
    const primary = rst.getPrimary();
    const expireAfterSeconds = shouldTruncateAllAfterUpgrade ? 1 : 3600;
    const {currentTimeForTimeBasedExpiration, _unused, shouldRetainDocs, allDocs, collectionsInfo} =
        setupTimeBasedPreImageRetentionPolicyTest(primary, primary, expireAfterSeconds);

    // Fix wall time used by pre image remover.
    setChangeStreamPreImageRemoverCurrentTimeAndWaitForAllNodes(rst,
                                                                currentTimeForTimeBasedExpiration);

    // Verify that when time-based pre-image expiration disabled, no pre-images are not deleted.
    verifyPreImagesForAllNodes(rst, [allDocs, allDocs], collectionsInfo);

    // Enable time based expiration.
    setExpireAfterSeconds(rst, expireAfterSeconds);

    // Both primary and secondary should remove expired documents.
    verifyPreImagesForAllNodes(rst, [shouldRetainDocs, shouldRetainDocs], collectionsInfo);

    // After upgrading the secondary to a version with truncate deletes, the preImages should be
    // deleted.
    rst.upgradePrimary(rst.getPrimary(), serverParametersForVersionString("latest"));

    const upgradedNodeExpectedDocs = shouldTruncateAllAfterUpgrade ? [] : shouldRetainDocs;
    verifyPreImagesForAllNodes(rst, [upgradedNodeExpectedDocs, shouldRetainDocs], collectionsInfo);

    rst.stopSet();
}

testPrimaryLatestSecondariesLastLts(/*shouldTruncateAllAfterUpgrade=*/ true);
testPrimaryLatestSecondariesLastLts(/*shouldTruncateAllAfterUpgrade=*/ false);

testPrimaryLastLtsSecondariesLatest(/*shouldTruncateAllAfterUpgrade=*/ true);
testPrimaryLastLtsSecondariesLatest(/*shouldTruncateAllAfterUpgrade=*/ false);
