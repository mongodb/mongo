/**
 * This tests that upgrading and downgrading FCV will unblock and reply to waiting hello
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

function runAwaitableHelloBeforeFCVChange(
    topologyVersionField, targetFCV, isPrimary, prevMinWireVersion, serverMaxWireVersion) {
    db.getMongo().setSecondaryOk();
    let response = assert.commandWorked(db.runCommand({
        hello: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
        internalClient:
            {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(serverMaxWireVersion)},
    }));

    // We only expect to increment the server TopologyVersion when the minWireVersion has changed.
    // This can only happen in two scenarios:
    // 1. Setting featureCompatibilityVersion from downgrading to fullyDowngraded.
    // 2. Setting featureCompatibilityVersion from fullyDowngraded to upgrading.
    assert.eq(topologyVersionField.counter + 1, response.topologyVersion.counter, response);
    const expectedHelloValue = isPrimary;
    const expectedSecondaryValue = !isPrimary;

    assert.eq(expectedHelloValue, response.isWritablePrimary, response);
    assert.eq(expectedSecondaryValue, response.secondary, response);

    const minWireVersion = response.minWireVersion;
    const maxWireVersion = response.maxWireVersion;
    assert.neq(prevMinWireVersion, minWireVersion);
    if (targetFCV === latestFCV) {
        // minWireVersion should always equal maxWireVersion if we have not fully downgraded FCV.
        assert.eq(minWireVersion, maxWireVersion, response);
    } else if (targetFCV === lastContinuousFCV) {
        assert.eq(minWireVersion + 1, maxWireVersion, response);
    } else {
        assert.eq(minWireVersion + numVersionsSinceLastLTS, maxWireVersion, response);
    }
}

function runTest(downgradeFCV) {
    jsTestLog("Running test with downgradeFCV: " + downgradeFCV);

    // This test manually runs hello with the 'internalClient' field, which means that to the
    // mongod, the connection appears to be from another server. This makes mongod to return an
    // hello response with a real 'minWireVersion' for internal clients instead of 0.
    //
    // The value of 'internalClient.maxWireVersion' in the hello command does not matter for the
    // purpose of this test and the hello command will succeed regardless because this is run
    // through the shell and the shell is always compatible talking to the server. In reality
    // though, a real internal client with a lower binary version is expected to hang up immediately
    // after receiving the response to the hello command from a latest server with an upgraded
    // FCV.
    //
    // And we need to use a side connection to do so in order to prevent the test connection from
    // being closed on FCV changes.
    function helloAsInternalClient() {
        let connInternal = new Mongo(primary.host);
        const res = assert.commandWorked(connInternal.adminCommand({
            hello: 1,
            internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(9)}
        }));
        connInternal.close();
        return res;
    }

    // Get the server topologyVersion, minWireVersion, and maxWireversion.
    const primaryResult = helloAsInternalClient();
    assert(primaryResult.hasOwnProperty("topologyVersion"), tojson(primaryResult));

    const maxWireVersion = primaryResult.maxWireVersion;
    const initMinWireVersion = primaryResult.minWireVersion;
    assert.eq(maxWireVersion, initMinWireVersion);

    const secondaryResult = assert.commandWorked(secondaryAdminDB.runCommand({hello: 1}));
    assert(secondaryResult.hasOwnProperty("topologyVersion"), tojson(secondaryResult));
    const primaryTopologyVersion = primaryResult.topologyVersion;
    assert(primaryTopologyVersion.hasOwnProperty("processId"), tojson(primaryTopologyVersion));
    assert(primaryTopologyVersion.hasOwnProperty("counter"), tojson(primaryTopologyVersion));

    const secondaryTopologyVersion = secondaryResult.topologyVersion;
    assert(secondaryTopologyVersion.hasOwnProperty("processId"), tojson(secondaryTopologyVersion));
    assert(secondaryTopologyVersion.hasOwnProperty("counter"), tojson(secondaryTopologyVersion));

    // A failpoint signalling that the servers have received the hello request and are waiting
    // for a topology change.
    let primaryFailPoint = configureFailPoint(primary, "waitForHelloResponse");
    let secondaryFailPoint = configureFailPoint(secondary, "waitForHelloResponse");

    // Send an awaitable hello request. This will block until maxAwaitTimeMS has elapsed or a
    // topology change happens.
    let awaitHelloBeforeDowngradeFCVOnPrimary =
        startParallelShell(funWithArgs(runAwaitableHelloBeforeFCVChange,
                                       primaryTopologyVersion,
                                       downgradeFCV,
                                       true /* isPrimary */,
                                       initMinWireVersion,
                                       maxWireVersion),
                           primary.port);
    let awaitHelloBeforeDowngradeFCVOnSecondary =
        startParallelShell(funWithArgs(runAwaitableHelloBeforeFCVChange,
                                       secondaryTopologyVersion,
                                       downgradeFCV,
                                       false /* isPrimary */,
                                       initMinWireVersion,
                                       maxWireVersion),
                           secondary.port);
    primaryFailPoint.wait();
    secondaryFailPoint.wait();

    // Each node has one hello request waiting on a topology change.
    let numAwaitingTopologyChangeOnPrimary =
        primaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    let numAwaitingTopologyChangeOnSecondary =
        secondaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(1, numAwaitingTopologyChangeOnPrimary);
    assert.eq(1, numAwaitingTopologyChangeOnSecondary);

    // Setting the FCV to the same version will not trigger an hello response.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(primaryAdminDB, latestFCV);
    checkFCV(secondaryAdminDB, latestFCV);

    // Each node still has one hello request waiting on a topology change.
    numAwaitingTopologyChangeOnPrimary =
        primaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    numAwaitingTopologyChangeOnSecondary =
        secondaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(1, numAwaitingTopologyChangeOnPrimary);
    assert.eq(1, numAwaitingTopologyChangeOnSecondary);

    jsTestLog("Downgrade the featureCompatibilityVersion.");
    // Downgrading the FCV will cause the hello requests to respond on both primary and
    // secondary.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    awaitHelloBeforeDowngradeFCVOnPrimary();
    awaitHelloBeforeDowngradeFCVOnSecondary();
    // Ensure the featureCompatibilityVersion document update has been replicated.
    rst.awaitReplication();
    checkFCV(primaryAdminDB, downgradeFCV);
    checkFCV(secondaryAdminDB, downgradeFCV);

    // All hello requests should have been responded to after the FCV change.
    numAwaitingTopologyChangeOnPrimary =
        primaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    numAwaitingTopologyChangeOnSecondary =
        secondaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(0, numAwaitingTopologyChangeOnPrimary);
    assert.eq(0, numAwaitingTopologyChangeOnSecondary);

    // Get the new topologyVersion.
    let primaryResponseAfterDowngrade = helloAsInternalClient();
    assert(primaryResponseAfterDowngrade.hasOwnProperty("topologyVersion"),
           tojson(primaryResponseAfterDowngrade));
    let primaryTopologyVersionAfterDowngrade = primaryResponseAfterDowngrade.topologyVersion;
    let minWireVersionAfterDowngrade = primaryResponseAfterDowngrade.minWireVersion;

    let secondaryResponseAfterDowngrade =
        assert.commandWorked(secondaryAdminDB.runCommand({hello: 1}));
    assert(secondaryResponseAfterDowngrade.hasOwnProperty("topologyVersion"),
           tojson(secondaryResponseAfterDowngrade));
    let secondaryTopologyVersionAfterDowngrade = secondaryResponseAfterDowngrade.topologyVersion;

    if (downgradeFCV === lastLTSFCV && lastLTSFCV !== lastContinuousFCV) {
        // Test upgrading from last-lts to last-continuous FCV. We allow this upgrade path via the
        // setFeatureCompatibilityVersion command with fromConfigServer: true.

        // Reconfigure the failpoint to refresh the number of times the failpoint has been entered.
        primaryFailPoint = configureFailPoint(primary, "waitForHelloResponse");
        secondaryFailPoint = configureFailPoint(secondary, "waitForHelloResponse");
        let awaitHelloBeforeUpgradeOnPrimary =
            startParallelShell(funWithArgs(runAwaitableHelloBeforeFCVChange,
                                           primaryTopologyVersionAfterDowngrade,
                                           lastContinuousFCV,
                                           true /* isPrimary */,
                                           minWireVersionAfterDowngrade,
                                           maxWireVersion),
                               primary.port);
        let awaitHelloBeforeUpgradeOnSecondary =
            startParallelShell(funWithArgs(runAwaitableHelloBeforeFCVChange,
                                           secondaryTopologyVersionAfterDowngrade,
                                           lastContinuousFCV,
                                           false /* isPrimary */,
                                           minWireVersionAfterDowngrade,
                                           maxWireVersion),
                               secondary.port);
        primaryFailPoint.wait();
        secondaryFailPoint.wait();

        // Each node has one hello request waiting on a topology change.
        numAwaitingTopologyChangeOnPrimary =
            primaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
        numAwaitingTopologyChangeOnSecondary =
            secondaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
        assert.eq(1, numAwaitingTopologyChangeOnPrimary);
        assert.eq(1, numAwaitingTopologyChangeOnSecondary);

        // Upgrade the FCV to last-continuous.
        assert.commandWorked(primaryAdminDB.runCommand(
            {setFeatureCompatibilityVersion: lastContinuousFCV, fromConfigServer: true}));
        awaitHelloBeforeUpgradeOnPrimary();
        awaitHelloBeforeUpgradeOnSecondary();

        // Ensure the featureCompatibilityVersion document update has been replicated.
        rst.awaitReplication();
        checkFCV(primaryAdminDB, lastContinuousFCV);
        checkFCV(secondaryAdminDB, lastContinuousFCV);

        // All hello requests should have been responded to after the FCV change.
        numAwaitingTopologyChangeOnPrimary =
            primaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
        numAwaitingTopologyChangeOnSecondary =
            secondaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
        assert.eq(0, numAwaitingTopologyChangeOnPrimary);
        assert.eq(0, numAwaitingTopologyChangeOnSecondary);

        // Reset the FCV back to last-lts and the get the new hello parameters.
        // We must upgrade to latestFCV first since downgrading from last-continuous to last-stable
        // is forbidden.
        assert.commandWorked(
            primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        assert.commandWorked(
            primaryAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
        rst.awaitReplication();
        checkFCV(primaryAdminDB, lastLTSFCV);
        checkFCV(secondaryAdminDB, lastLTSFCV);

        primaryResponseAfterDowngrade = helloAsInternalClient();
        assert(primaryResponseAfterDowngrade.hasOwnProperty("topologyVersion"),
               tojson(primaryResponseAfterDowngrade));
        primaryTopologyVersionAfterDowngrade = primaryResponseAfterDowngrade.topologyVersion;
        minWireVersionAfterDowngrade = primaryResponseAfterDowngrade.minWireVersion;

        secondaryResponseAfterDowngrade =
            assert.commandWorked(secondaryAdminDB.runCommand({hello: 1}));
        assert(secondaryResponseAfterDowngrade.hasOwnProperty("topologyVersion"),
               tojson(secondaryResponseAfterDowngrade));
        secondaryTopologyVersionAfterDowngrade = secondaryResponseAfterDowngrade.topologyVersion;
    }

    // Reconfigure the failpoint to refresh the number of times the failpoint has been entered.
    primaryFailPoint = configureFailPoint(primary, "waitForHelloResponse");
    secondaryFailPoint = configureFailPoint(secondary, "waitForHelloResponse");
    let awaitHelloBeforeUpgradeFCVOnPrimary =
        startParallelShell(funWithArgs(runAwaitableHelloBeforeFCVChange,
                                       primaryTopologyVersionAfterDowngrade,
                                       latestFCV,
                                       true /* isPrimary */,
                                       minWireVersionAfterDowngrade,
                                       maxWireVersion),
                           primary.port);
    let awaitHelloBeforeUpgradeFCVOnSecondary =
        startParallelShell(funWithArgs(runAwaitableHelloBeforeFCVChange,
                                       secondaryTopologyVersionAfterDowngrade,
                                       latestFCV,
                                       false /* isPrimary */,
                                       minWireVersionAfterDowngrade,
                                       maxWireVersion),
                           secondary.port);
    primaryFailPoint.wait();
    secondaryFailPoint.wait();

    // Each node has one hello request waiting on a topology change.
    numAwaitingTopologyChangeOnPrimary =
        primaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    numAwaitingTopologyChangeOnSecondary =
        secondaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(1, numAwaitingTopologyChangeOnPrimary);
    assert.eq(1, numAwaitingTopologyChangeOnSecondary);

    // Setting the FCV to the same version will not trigger an hello response.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    checkFCV(primaryAdminDB, downgradeFCV);
    checkFCV(secondaryAdminDB, downgradeFCV);

    // Each node still has one hello request waiting on a topology change.
    numAwaitingTopologyChangeOnPrimary =
        primaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    numAwaitingTopologyChangeOnSecondary =
        secondaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(1, numAwaitingTopologyChangeOnPrimary);
    assert.eq(1, numAwaitingTopologyChangeOnSecondary);

    jsTestLog("Upgrade the featureCompatibilityVersion.");
    // Upgrading the FCV will cause the hello requests to respond on both primary and secondary.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    awaitHelloBeforeUpgradeFCVOnPrimary();
    awaitHelloBeforeUpgradeFCVOnSecondary();
    // Ensure the featureCompatibilityVersion document update has been replicated.
    rst.awaitReplication();
    checkFCV(primaryAdminDB, latestFCV);
    checkFCV(secondaryAdminDB, latestFCV);

    // All hello requests should have been responded to after the FCV change.
    numAwaitingTopologyChangeOnPrimary =
        primaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    numAwaitingTopologyChangeOnSecondary =
        secondaryAdminDB.serverStatus().connections.awaitingTopologyChanges;
    assert.eq(0, numAwaitingTopologyChangeOnPrimary);
    assert.eq(0, numAwaitingTopologyChangeOnSecondary);
}

runTest(lastLTSFCV);
runTest(lastContinuousFCV);

rst.stopSet();
})();
