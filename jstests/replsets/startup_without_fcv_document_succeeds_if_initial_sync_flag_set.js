/**
 * Tests that when a node restarts during initial sync before it can clone the FCV document, it is
 * still able to start up successfully and restart initial sync.
 */

(function() {
    load("jstests/libs/check_log.js");
    load("jstests/libs/feature_compatibility_version.js");

    rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    jsTestLog("Adding a second node to the replica set.");

    const adminDbName = "admin";
    const versionCollName = "system.version";
    const nss = adminDbName + "." + versionCollName;

    // Hang initial sync before cloning the FCV document.
    let secondary = rst.add({rsConfig: {priority: 0}});
    assert.commandWorked(secondary.getDB('admin').runCommand({
        configureFailPoint: 'initialSyncHangCollectionClonerBeforeEstablishingCursor',
        mode: 'alwaysOn',
        data: {nss: nss}
    }));
    rst.reInitiate();
    checkLog.contains(
        secondary,
        "initialSyncHangCollectionClonerBeforeEstablishingCursor fail point enabled for admin.");

    jsTestLog("Restarting secondary in the early stages of initial sync.");

    rst.restart(secondary);
    rst.awaitSecondaryNodes();

    // Get the new secondary connection.
    secondary = rst.getSecondary();
    secondary.setSlaveOk(true);

    const secondaryAdminDb = secondary.getDB("admin");
    // Assert that the FCV document was cloned through initial sync on the secondary.
    checkFCV(secondaryAdminDb, latestFCV);
    rst.stopSet();
}());
