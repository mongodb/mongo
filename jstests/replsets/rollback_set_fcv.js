/*
 * Tests the following scenarios where the featureCompatibilityVersion document is rolled back and
 * verify that the in-memory and on-disk FCV stay consistent.
 * - the FCV document is rolled back from fully upgraded to upgrading
 * - the FCV document is rolled back from upgrading to fully downgraded
 * - the FCV document is rolled back from fully downgraded to downgrading
 * - the FCV document is rolled back from downgrading to fully upgraded
 *
 *  @tags: [multiversion_incompatible]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

function setFCV(fcv) {
    assert.commandFailedWithCode(
        db.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}),
        ErrorCodes.InterruptedDueToReplStateChange,
    );
}

// Using getParameter results in waiting for the current FCV to be majority committed.  In this
// test, it never will, so we need to get the FCV directly.
function getFCVFromDocument(conn) {
    return conn.getDB("admin").system.version.find().readConcern("local").toArray()[0];
}

function getTopologyVersion(node) {
    // We need to use a new connection here because we run an internalClient command, which
    // will make the connection be marked as internal and leads to following commands fail.
    let connInternal = new Mongo(node.host);
    const res = assert.commandWorked(
        connInternal.adminCommand({
            hello: 1,
            internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(9)},
        }),
    );
    connInternal.close();
    return res.topologyVersion;
}

// fromFCV refers to the FCV we will test rolling back from.
// toFCV refers to the FCV we will test rolling back to.
function rollbackFCVFromDowngradingOrUpgrading(fromFCV, toFCV) {
    let primary = rollbackTest.getPrimary();
    let secondary = rollbackTest.getSecondary();
    let primaryAdminDB = primary.getDB("admin");
    let secondaryAdminDB = secondary.getDB("admin");

    // Ensure the cluster starts at the correct FCV.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: toFCV, confirm: true}));
    // Wait until the config has propagated to the other nodes and the primary has learned of it, so
    // that the config replication check in 'setFeatureCompatibilityVersion' is satisfied. This is
    // only important since 'setFeatureCompatibilityVersion' is known to implicitly call internal
    // reconfigs as part of upgrade/downgrade behavior.
    rollbackTest.getTestFixture().waitForConfigReplication(primary);
    // Wait for the majority commit point to be updated on the secondary, because checkFCV calls
    // getParameter for the featureCompatibilityVersion, which will wait until the FCV change makes
    // it into the node's majority committed snapshot.
    rollbackTest.getTestFixture().awaitLastOpCommitted(undefined /* timeout */, [secondary]);

    jsTestLog(
        "Testing rolling back FCV from {version: " +
            lastLTSFCV +
            ", targetVersion: " +
            fromFCV +
            "} to {version: " +
            toFCV +
            "}",
    );

    rollbackTest.transitionToRollbackOperations();
    let setFCVInParallel = startParallelShell(funWithArgs(setFCV, fromFCV), primary.port);
    // Wait for the FCV update to be reflected on the primary. This should eventually be rolled
    // back.
    assert.soon(function () {
        let featureCompatibilityVersion = getFCVFromDocument(primary);
        return featureCompatibilityVersion.hasOwnProperty("targetVersion");
    }, "Failed waiting for the server to set the targetVersion: " + fromFCV);
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    // Secondaries should never have received the FCV update.
    checkFCV(secondaryAdminDB, toFCV);
    const topologyVersionBeforeRollback = getTopologyVersion(primary);

    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    setFCVInParallel();
    rollbackTest.transitionToSteadyStateOperations();
    const topologyVersionAfterRollback = getTopologyVersion(primary);
    // There should be 3 topology version changes without FCV change when we transition to
    // kSyncSourceOpsDuringRollback and kSteadyStateOps, including reconnect node, transition from
    // primary to rollback and transition from rollback to secondary. If the FCV change also
    // triggers a topology version change, then the topology version gap between before and after
    // rollback should be 4.
    const topologyVersionDiff = 4;
    assert.eq(topologyVersionBeforeRollback.counter + topologyVersionDiff, topologyVersionAfterRollback.counter);
    // The primary should have rolled back their FCV to be consistent with the rest of the replica
    // set.
    checkFCV(primaryAdminDB, toFCV);
    checkFCV(secondaryAdminDB, toFCV);

    let newPrimary = rollbackTest.getPrimary();
    // As a rule, we forbid downgrading a node while a node is still in the upgrading state and
    // vice versa. Ensure that the in-memory and on-disk FCV are consistent by checking that we are
    // able to set the FCV back to the original version.
    assert.commandWorked(newPrimary.adminCommand({setFeatureCompatibilityVersion: toFCV, confirm: true}));
}

// fromFCV refers to the FCV we will test rolling back from.
// toFCV refers to the FCV we will test rolling back to.
function rollbackFCVFromDowngradedOrUpgraded(fromFCV, toFCV, failPoint) {
    let primary = rollbackTest.getPrimary();
    let secondary = rollbackTest.getSecondary();
    let primaryAdminDB = primary.getDB("admin");
    let secondaryAdminDB = secondary.getDB("admin");

    // Complete the upgrade/downgrade to ensure we are not in the upgrading/downgrading state.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: toFCV, confirm: true}));
    // Wait for the majority commit point to be updated on the secondary, because checkFCV calls
    // getParameter for the featureCompatibilityVersion, which will wait until the FCV change makes
    // it into the node's majority committed snapshot.
    rollbackTest.getTestFixture().awaitLastOpCommitted(undefined /* timeout */, [secondary]);

    jsTestLog(
        "Testing rolling back FCV from {version: " +
            fromFCV +
            "} to {version: " +
            lastLTSFCV +
            ", targetVersion: " +
            fromFCV +
            "}",
    );

    // A failpoint to hang right before unsetting the targetVersion.
    const hangBeforeUnsettingTargetVersion = configureFailPoint(primary, failPoint);
    let setFCVInParallel = startParallelShell(funWithArgs(setFCV, fromFCV), primary.port);
    hangBeforeUnsettingTargetVersion.wait();
    rollbackTest.transitionToRollbackOperations();
    // Turn off the failpoint so the primary will proceed to unset the targetVersion. This update
    // should never make it to the secondary.
    hangBeforeUnsettingTargetVersion.off();
    assert.soon(function () {
        let featureCompatibilityVersion = getFCVFromDocument(primary);
        return (
            !featureCompatibilityVersion.hasOwnProperty("targetVersion") &&
            featureCompatibilityVersion.version === fromFCV
        );
    }, "Failed waiting for server to unset the targetVersion or to set the FCV to " + fromFCV);
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    // The secondary should never have received the update to unset the targetVersion.
    if (fromFCV == lastLTSFCV || FeatureFlagUtil.isPresentAndEnabled(primary, "UpgradingToDowngrading")) {
        // When downgrading, the secondary should still be in isCleaningServerMetadata.
        checkFCV(secondaryAdminDB, lastLTSFCV, fromFCV, true /* isCleaningServerMetadata */);
    } else {
        checkFCV(secondaryAdminDB, lastLTSFCV, fromFCV);
    }

    const topologyVersionBeforeRollback = getTopologyVersion(primary);

    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    setFCVInParallel();
    rollbackTest.transitionToSteadyStateOperations();
    const topologyVersionAfterRollback = getTopologyVersion(primary);
    // There should be 3 topology version changes without FCV change when we transition to
    // kSyncSourceOpsDuringRollback and kSteadyStateOps, including reconnect node, transition from
    // primary to rollback and transition from rollback to secondary. If the FCV change also
    // triggers a topology version change, then the topology version gap between before and after
    // rollback should be 4.
    const topologyVersionDiff = 4;
    assert.eq(topologyVersionBeforeRollback.counter + topologyVersionDiff, topologyVersionAfterRollback.counter);
    // The primary should have rolled back their FCV to contain the targetVersion.
    if (fromFCV == lastLTSFCV || FeatureFlagUtil.isPresentAndEnabled(primary, "UpgradingToDowngrading")) {
        // Rolling back from downgraded to isCleaningServerMetadata state.
        checkFCV(primaryAdminDB, lastLTSFCV, fromFCV, true /* isCleaningServerMetadata */);
        checkFCV(secondaryAdminDB, lastLTSFCV, fromFCV, true /* isCleaningServerMetadata */);
    } else {
        checkFCV(primaryAdminDB, lastLTSFCV, fromFCV);
        checkFCV(secondaryAdminDB, lastLTSFCV, fromFCV);
    }

    let newPrimary = rollbackTest.getPrimary();
    // As a rule, we forbid downgrading a node while a node is still in the upgrading state and
    // vice versa.
    // With the downgrading to upgrading path, we do not permit upgrading if we are cleaning
    // server metadata.
    // With the upgrading to downgrading path, we do not permit downgrading if we are cleaning
    // server metadata.
    // Ensure that the in-memory and on-disk FCV are consistent by checking that this rule is
    // upheld after rollback.
    if (fromFCV === lastLTSFCV && toFCV === latestFCV) {
        assert.commandFailedWithCode(
            newPrimary.adminCommand({setFeatureCompatibilityVersion: toFCV, confirm: true}),
            7428200,
        );
    } else if (
        FeatureFlagUtil.isPresentAndEnabled(primary, "UpgradingToDowngrading") &&
        fromFCV === latestFCV &&
        (toFCV === lastLTSFCV || toFCV === lastContinuousFCV)
    ) {
        assert.commandFailedWithCode(
            newPrimary.adminCommand({setFeatureCompatibilityVersion: toFCV, confirm: true}),
            10778001,
        );
    } else {
        assert.commandFailedWithCode(
            newPrimary.adminCommand({setFeatureCompatibilityVersion: toFCV, confirm: true}),
            5147403,
        );
    }
}

// 1) Test rolling back from upgrading to downgrading.
//     -Start off with downgrading from latest to lastLTS.
//     -Go to upgrading from lastLTS to latest state.
//     -Rollback and make sure the FCV doc is back in the downgrading from latest to lastLTS state.
// 2) Test rolling back from downgrading to upgrading.
//     -Start off with upgrading from lastLTS to latest.
//     -Go to downgrading from latest to lastLTS state.
//     -Rollback and make sure the FCV doc is back in the upgrading from lastLTS to latest state.

function rollbackFCV_FromUpgradingToDowngrading_or_FromDowngradingToUpgrading(fromFCV, toFCV) {
    let fcvDoc;
    const rollbackNode = rollbackTest.getPrimary();
    const syncSource = rollbackTest.getSecondary();
    const rollbackNodeAdminDB = rollbackNode.getDB("admin");
    const syncSourceAdminDB = syncSource.getDB("admin");

    // Ensure the cluster starts at the correct FCV.
    assert.commandWorked(rollbackNode.adminCommand({setFeatureCompatibilityVersion: fromFCV, confirm: true}));

    fcvDoc = rollbackNodeAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog(`rollbackNode's version at start: ${tojson(fcvDoc)}`);
    checkFCV(rollbackNodeAdminDB, fromFCV);

    // Set the failpoints so that both upgrading and downgrading would fail.
    assert.commandWorked(rollbackNode.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));
    assert.commandWorked(rollbackNode.adminCommand({configureFailPoint: "failUpgrading", mode: "alwaysOn"}));

    // Go to initial transitioning state (either upgrading or downgrading).
    let initialTransitionType = MongoRunner.compareBinVersions(fromFCV, toFCV) === 1 ? "downgrading" : "upgrading";
    const finalTransitionType = initialTransitionType === "downgrading" ? "upgrading" : "downgrading";
    const effectiveFCV = initialTransitionType === "downgrading" ? toFCV : fromFCV;
    assert.commandFailed(rollbackNodeAdminDB.runCommand({setFeatureCompatibilityVersion: toFCV, confirm: true}));

    fcvDoc = rollbackNodeAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog(`rollbackNode's version after ${initialTransitionType}: ${tojson(fcvDoc)}`);
    checkFCV(rollbackNodeAdminDB, effectiveFCV, toFCV);

    // Wait until the config has propagated to the other nodes and the rollbackNode has learned of
    // it, so that the config replication check in 'setFeatureCompatibilityVersion' is satisfied.
    // This is only important since 'setFeatureCompatibilityVersion' is known to implicitly call
    // internal reconfigs as part of upgrade/downgrade behavior.
    rollbackTest.getTestFixture().waitForConfigReplication(rollbackNode);
    // Wait for the majority commit point to be updated on the sync source, because checkFCV calls
    // getParameter for the featureCompatibilityVersion, which will wait until the FCV change makes
    // it into the node's majority committed snapshot.
    rollbackTest.getTestFixture().awaitLastOpCommitted(undefined /* timeout */, [syncSource]);

    // Test rolling back either from upgrading to downgrading or from downgrading to upgrading
    jsTestLog(
        `Testing rolling back FCV from {version: ${toFCV}, targetVersion: ${fromFCV}} ` +
            `to {version: ${effectiveFCV}, targetVersion: ${toFCV}}`,
    );

    rollbackTest.transitionToRollbackOperations();
    let setFCVInParallel = startParallelShell(funWithArgs(setFCV, fromFCV), rollbackNode.port);

    // Wait for the FCV update to be reflected on the rollbackNode. This should eventually be rolled
    // back.
    assert.soon(function () {
        let featureCompatibilityVersion = getFCVFromDocument(rollbackNode);
        jsTestLog(
            `rollbackNode's version in parallel shell (should eventually be ${
                finalTransitionType
            }): ${tojson(featureCompatibilityVersion)}`,
        );
        return (
            featureCompatibilityVersion.hasOwnProperty("targetVersion") &&
            featureCompatibilityVersion.targetVersion === fromFCV &&
            (finalTransitionType === "upgrading"
                ? !featureCompatibilityVersion.hasOwnProperty("previousVersion")
                : featureCompatibilityVersion.hasOwnProperty("previousVersion"))
        );
    }, `Failed waiting for the server to set the target version ${fromFCV}`);

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

    setFCVInParallel();

    fcvDoc = rollbackNodeAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog(`Rollback node's version after setFCVInParallel: ${tojson(fcvDoc)}`);
    // Since the rollback node is isolated and the FCV changes are not guaranteed to be visible
    // in the majority committed snapshot, we only check the node's FCV document values directly.
    assert.eq(fcvDoc.version, effectiveFCV, `FCV document 'version' does not match: ${tojson(fcvDoc)}`);
    assert.eq(fcvDoc.targetVersion, fromFCV, `FCV document 'targetVersion' does not match: ${tojson(fcvDoc)}`);
    assert.eq(
        fcvDoc.previousVersion,
        effectiveFCV === toFCV ? undefined : toFCV,
        `FCV server parameter 'previousVersion' does not match: ${tojson(fcvDoc)}`,
    );

    // Secondary should never have received the FCV update.
    fcvDoc = syncSourceAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog(`syncSource's version (should still be ${initialTransitionType}): ${tojson(fcvDoc)}`);
    checkFCV(syncSourceAdminDB, effectiveFCV, toFCV);

    const topologyVersionBeforeRollback = getTopologyVersion(rollbackNode);
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();
    const topologyVersionAfterRollback = getTopologyVersion(rollbackNode);

    // There should be 3 topology version changes without FCV change when we transition to
    // kSyncSourceOpsDuringRollback and kSteadyStateOps, including reconnect node, transition from
    // primary to rollback and transition from rollback to secondary. When rollback from
    // upgrading to downgrading, FCV change should not increment topology version, but when rollback
    // from downgrading to upgrading, topology version changes should be incremented by 1
    // TODO(SERVER-109377): Explain why the topologyVersion gets incremented one more time in that
    // case

    const topologyVersionDiff = initialTransitionType === "downgrading" ? 3 : 4;
    assert.eq(topologyVersionBeforeRollback.counter + topologyVersionDiff, topologyVersionAfterRollback.counter);

    // The rollbackNode should have rolled back their FCV to be consistent with the rest of the
    // replica set.
    fcvDoc = rollbackNodeAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog(`rollbackNode's version after rollback: ${tojson(fcvDoc)}`);
    checkFCV(rollbackNodeAdminDB, effectiveFCV, toFCV);
    fcvDoc = syncSourceAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog(`SyncSource's version after rollback: ${tojson(fcvDoc)}`);
    checkFCV(syncSourceAdminDB, effectiveFCV, toFCV);

    const newPrimary = rollbackTest.getPrimary();
    const newPrimaryAdminDB = newPrimary.getDB("admin");

    // We should now be able to set the FCV from downgrading to upgrading to upgraded or from
    // upgrading to downgrading to downgraded.
    assert.commandWorked(newPrimary.adminCommand({setFeatureCompatibilityVersion: fromFCV, confirm: true}));
    checkFCV(newPrimaryAdminDB, fromFCV);

    assert.commandWorked(rollbackNode.adminCommand({configureFailPoint: "failDowngrading", mode: "off"}));
    assert.commandWorked(rollbackNode.adminCommand({configureFailPoint: "failUpgrading", mode: "off"}));
}

function rollbackFCVFromIsCleaningServerMetadataToTransitioning(fromFCV, toFCV) {
    let primary = rollbackTest.getPrimary();
    let secondary = rollbackTest.getSecondary();
    let primaryAdminDB = primary.getDB("admin");
    let secondaryAdminDB = secondary.getDB("admin");

    // Complete the upgrade/downgrade to ensure we are not in the upgrading/downgrading state.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: fromFCV, confirm: true}));
    // Wait for the majority commit point to be updated on the secondary, because checkFCV calls
    // getParameter for the featureCompatibilityVersion, which will wait until the FCV change makes
    // it into the node's majority committed snapshot.
    rollbackTest.getTestFixture().awaitLastOpCommitted(undefined /* timeout */, [secondary]);

    let transitionType = MongoRunner.compareBinVersions(fromFCV, toFCV) === 1 ? "Downgrading" : "Upgrading";
    jsTestLog(`Testing rolling back FCV from isCleaningServerMetadata state to ${transitionType} state`);
    const effectiveFCV = transitionType === "Downgrading" ? toFCV : fromFCV;

    // A failpoint to hang right before setting isCleaningServerMetadata.
    const hangTransitionBeforeIsCleaningServerMetadata = configureFailPoint(
        primary,
        "hangTransitionBeforeIsCleaningServerMetadata",
    );
    let setFCVInParallel = startParallelShell(funWithArgs(setFCV, toFCV), primary.port);
    hangTransitionBeforeIsCleaningServerMetadata.wait();
    rollbackTest.transitionToRollbackOperations();
    // Turn off the failpoint so the primary will proceed to set isCleaningServerMetadata. This
    // update should never make it to the secondary.
    hangTransitionBeforeIsCleaningServerMetadata.off();
    assert.soon(function () {
        let featureCompatibilityVersion = getFCVFromDocument(primary);
        return (
            featureCompatibilityVersion.hasOwnProperty("targetVersion") &&
            featureCompatibilityVersion.hasOwnProperty("isCleaningServerMetadata") &&
            featureCompatibilityVersion.targetVersion === toFCV &&
            featureCompatibilityVersion.isCleaningServerMetadata === true &&
            featureCompatibilityVersion.version === effectiveFCV
        );
    }, "Failed waiting for server to enter isCleaningServerMetadata state");
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    // The secondary should never have received the update to set isCleaningServerMetadata
    checkFCV(secondaryAdminDB, effectiveFCV, toFCV);

    const topologyVersionBeforeRollback = getTopologyVersion(primary);
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    setFCVInParallel();
    rollbackTest.transitionToSteadyStateOperations();
    const topologyVersionAfterRollback = getTopologyVersion(primary);
    // There should be 3 topology version changes without FCV change when we transition to
    // kSyncSourceOpsDuringRollback and kSteadyStateOps, including reconnect node, transition from
    // primary to rollback and transition from rollback to secondary. When rollback from
    // isCleaningServerMetadata to downgrading/upgrading, FCV change should not increment topology
    // version.
    const topologyVersionDiff = 3;
    assert.eq(topologyVersionBeforeRollback.counter + topologyVersionDiff, topologyVersionAfterRollback.counter);

    let newPrimary = rollbackTest.getPrimary();
    // Test transition post-rollback.
    assert.commandWorked(newPrimary.adminCommand({setFeatureCompatibilityVersion: fromFCV, confirm: true}));
}

const testName = jsTest.name();

const rollbackTest = new RollbackTest(testName);

// Tests the case where we roll back the FCV state from downgrading to fully upgraded.
rollbackFCVFromDowngradingOrUpgrading(lastLTSFCV, latestFCV);

// Tests the case where we roll back the FCV state from upgrading to fully downgraded.
rollbackFCVFromDowngradingOrUpgrading(latestFCV, lastLTSFCV);

// Tests the case where we roll back the FCV state from fully downgraded to downgrading (while in
// isCleaningServerMetadata state).
rollbackFCVFromDowngradedOrUpgraded(lastLTSFCV, latestFCV, "hangBeforeTransitioningToDowngraded");

// Tests the case where we roll back the FCV state from fully upgraded to upgrading.
rollbackFCVFromDowngradedOrUpgraded(latestFCV, lastLTSFCV, "hangWhileUpgrading");

// Tests the case where we roll back the FCV state from upgrading to downgrading.
rollbackFCV_FromUpgradingToDowngrading_or_FromDowngradingToUpgrading(latestFCV, lastLTSFCV);

// Tests roll back from isCleaningServerMetadata to downgrading.
rollbackFCVFromIsCleaningServerMetadataToTransitioning(latestFCV, lastLTSFCV);

if (FeatureFlagUtil.isPresentAndEnabled(rollbackTest.getPrimary(), "UpgradingToDowngrading")) {
    // Tests the case where we roll back the FCV state from downgrading to upgrading.
    rollbackFCV_FromUpgradingToDowngrading_or_FromDowngradingToUpgrading(lastLTSFCV, latestFCV);

    // Tests roll back from isCleaningServerMetadata to upgrading.
    rollbackFCVFromIsCleaningServerMetadataToTransitioning(lastLTSFCV, latestFCV);
}

rollbackTest.stop();
