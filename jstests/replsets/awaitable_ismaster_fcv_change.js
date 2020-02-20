/**
 * This tests that upgrading and downgrading FCV will unblock and reply to waiting isMaster
 * requests.
 *
 * @tags: [multiversion_incompatible]
 */

(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryAdminDB = primary.getDB("admin");
const secondaryAdminDB = secondary.getDB("admin");

// This test manually runs isMaster with internalClient, which means that to the mongod, the
// connection appears to be from another server. Since mongod expects other cluster members to
// always include explicit read/write concern (on commands that accept read/write concern), this
// test must be careful to mimic this behavior.

// Get the server topologyVersion, minWireVersion, and maxWireversion.
const primaryResult = assert.commandWorked(primaryAdminDB.runCommand(
    {isMaster: 1, internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(9)}}));
assert(primaryResult.hasOwnProperty("topologyVersion"), tojson(primaryResult));
const primaryTopologyVersion = primaryResult.topologyVersion;
assert(primaryTopologyVersion.hasOwnProperty("processId"), tojson(primaryTopologyVersion));
assert(primaryTopologyVersion.hasOwnProperty("counter"), tojson(primaryTopologyVersion));

const maxWireVersion = primaryResult.maxWireVersion;
const initMinWireVersion = primaryResult.minWireVersion;
assert.eq(maxWireVersion, initMinWireVersion);

const secondaryResult = assert.commandWorked(secondaryAdminDB.runCommand({isMaster: 1}));
assert(secondaryResult.hasOwnProperty("topologyVersion"), tojson(secondaryResult));
const secondaryTopologyVersion = secondaryResult.topologyVersion;
assert(secondaryTopologyVersion.hasOwnProperty("processId"), tojson(secondaryTopologyVersion));
assert(secondaryTopologyVersion.hasOwnProperty("counter"), tojson(secondaryTopologyVersion));

function runAwaitableIsMasterBeforeFCVChange(
    topologyVersionField, isUpgrade, isPrimary, prevMinWireVersion, serverMaxWireVersion) {
    db.getMongo().setSlaveOk();
    let response = assert.commandWorked(db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
        internalClient:
            {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(serverMaxWireVersion)},
    }));

    // On downgrade from 4.4 to 4.2, the primary will reconfig the replset and signal isMaster.
    if (prevMinWireVersion === response.minWireVersion) {
        jsTestLog("Min wire version didn't change: " + prevMinWireVersion + ". Retry isMaster.");
        topologyVersionField = response.topologyVersion;
        response = assert.commandWorked(db.runCommand({
            isMaster: 1,
            topologyVersion: topologyVersionField,
            maxAwaitTimeMS: 99999999,
            internalClient:
                {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(serverMaxWireVersion)},
        }));
    }
    // We only expect to increment the server TopologyVersion when the minWireVersion has changed.
    // This can only happen in two scenarios:
    // 1. Setting featureCompatibilityVersion from downgrading to fullyDowngraded.
    // 2. Setting featureCompatibilityVersion from fullyDowngraded to upgrading.
    assert.eq(topologyVersionField.counter + 1, response.topologyVersion.counter, response);
    const expectedIsMasterValue = isPrimary;
    const expectedSecondaryValue = !isPrimary;

    assert.eq(expectedIsMasterValue, response.ismaster, response);
    assert.eq(expectedSecondaryValue, response.secondary, response);

    const minWireVersion = response.minWireVersion;
    const maxWireVersion = response.maxWireVersion;
    assert.neq(prevMinWireVersion, minWireVersion);
    if (isUpgrade) {
        // minWireVersion should always equal maxWireVersion if we have not fully downgraded FCV.
        assert.eq(minWireVersion, maxWireVersion, response);
    } else {
        assert.eq(minWireVersion + 1, maxWireVersion, response);
    }
}

// A failpoint signalling that the servers have received the isMaster request and are waiting for a
// topology change.
let primaryFailPoint = configureFailPoint(primary, "waitForIsMasterResponse");
let secondaryFailPoint = configureFailPoint(secondary, "waitForIsMasterResponse");

// Send an awaitable isMaster request. This will block until maxAwaitTimeMS has elapsed or a
// topology change happens.
let awaitIsMasterBeforeDowngradeFCVOnPrimary =
    startParallelShell(funWithArgs(runAwaitableIsMasterBeforeFCVChange,
                                   primaryTopologyVersion,
                                   false /* isUpgrade */,
                                   true /* isPrimary */,
                                   initMinWireVersion,
                                   maxWireVersion),
                       primary.port);
let awaitIsMasterBeforeDowngradeFCVOnSecondary =
    startParallelShell(funWithArgs(runAwaitableIsMasterBeforeFCVChange,
                                   secondaryTopologyVersion,
                                   false /* isUpgrade */,
                                   false /* isPrimary */,
                                   initMinWireVersion,
                                   maxWireVersion),
                       secondary.port);
primaryFailPoint.wait();
secondaryFailPoint.wait();

// Setting the FCV to the same version will not trigger an isMaster response.
assert.commandWorked(
    primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, writeConcern: {w: 1}}));
checkFCV(primaryAdminDB, latestFCV);
checkFCV(secondaryAdminDB, latestFCV);

jsTestLog("Downgrade the featureCompatibilityVersion.");
// Downgrading the FCV will cause the isMaster requests to respond on both primary and secondary.
assert.commandWorked(primaryAdminDB.runCommand(
    {setFeatureCompatibilityVersion: lastStableFCV, writeConcern: {w: 1}}));
awaitIsMasterBeforeDowngradeFCVOnPrimary();
awaitIsMasterBeforeDowngradeFCVOnSecondary();
// Ensure the featureCompatibilityVersion document update has been replicated.
rst.awaitReplication();
checkFCV(primaryAdminDB, lastStableFCV);
checkFCV(secondaryAdminDB, lastStableFCV);

// Get the new topologyVersion.
const primaryResponseAfterDowngrade = assert.commandWorked(primaryAdminDB.runCommand({
    isMaster: 1,
    internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(maxWireVersion)}
}));
assert(primaryResponseAfterDowngrade.hasOwnProperty("topologyVersion"),
       tojson(primaryResponseAfterDowngrade));
const primaryTopologyVersionAfterDowngrade = primaryResponseAfterDowngrade.topologyVersion;
const minWireVersionAfterDowngrade = primaryResponseAfterDowngrade.minWireVersion;

const secondaryResponseAfterDowngrade =
    assert.commandWorked(secondaryAdminDB.runCommand({isMaster: 1}));
assert(secondaryResponseAfterDowngrade.hasOwnProperty("topologyVersion"),
       tojson(secondaryResponseAfterDowngrade));
const secondaryTopologyVersionAfterDowngrade = secondaryResponseAfterDowngrade.topologyVersion;

// Reconfigure the failpoint to refresh the number of times the failpoint has been entered.
primaryFailPoint = configureFailPoint(primary, "waitForIsMasterResponse");
secondaryFailPoint = configureFailPoint(secondary, "waitForIsMasterResponse");
let awaitIsMasterBeforeUpgradeFCVOnPrimary =
    startParallelShell(funWithArgs(runAwaitableIsMasterBeforeFCVChange,
                                   primaryTopologyVersionAfterDowngrade,
                                   true /* isUpgrade */,
                                   true /* isPrimary */,
                                   minWireVersionAfterDowngrade,
                                   maxWireVersion),
                       primary.port);
let awaitIsMasterBeforeUpgradeFCVOnSecondary =
    startParallelShell(funWithArgs(runAwaitableIsMasterBeforeFCVChange,
                                   secondaryTopologyVersionAfterDowngrade,
                                   true /* isUpgrade */,
                                   false /* isPrimary */,
                                   minWireVersionAfterDowngrade,
                                   maxWireVersion),
                       secondary.port);
primaryFailPoint.wait();
secondaryFailPoint.wait();

// Setting the FCV to the same version will not trigger an isMaster response.
assert.commandWorked(primaryAdminDB.runCommand(
    {setFeatureCompatibilityVersion: lastStableFCV, writeConcern: {w: 1}}));
checkFCV(primaryAdminDB, lastStableFCV);
checkFCV(secondaryAdminDB, lastStableFCV);

jsTestLog("Upgrade the featureCompatibilityVersion.");
// Upgrading the FCV will cause the isMaster requests to respond on both primary and secondary.
assert.commandWorked(
    primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, writeConcern: {w: 1}}));
awaitIsMasterBeforeUpgradeFCVOnPrimary();
awaitIsMasterBeforeUpgradeFCVOnSecondary();
// Ensure the featureCompatibilityVersion document update has been replicated.
rst.awaitReplication();
checkFCV(primaryAdminDB, latestFCV);
checkFCV(secondaryAdminDB, latestFCV);

rst.stopSet();
})();
