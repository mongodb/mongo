/**
 * Tests that when a node restarts during initial sync before it can clone the FCV document, it is
 * still able to start up successfully and restart initial sync.
 *
 * This test asserts a certain FCV is cloned through initial sync.
 * @tags: [multiversion_incompatible]
 */

(function() {

load("jstests/libs/fail_point_util.js");

rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

jsTestLog("Adding a second node to the replica set.");

const adminDbName = "admin";
const versionCollName = "system.version";
const nss = adminDbName + "." + versionCollName;

// Hang initial sync before cloning the FCV document.
let secondary = rst.add({rsConfig: {priority: 0, votes: 0}});
let failPoint = configureFailPoint(secondary,
                                   'hangBeforeClonerStage',
                                   {cloner: 'CollectionCloner', stage: 'count', namespace: nss});
rst.reInitiate();
failPoint.wait();

jsTestLog("Restarting secondary in the early stages of initial sync.");
rst.restart(secondary);

rst.awaitSecondaryNodes();

// Get the new secondary connection.
secondary = rst.getSecondary();
secondary.setSecondaryOk();

const secondaryAdminDb = secondary.getDB("admin");
// Assert that the FCV document was cloned through initial sync on the secondary.
checkFCV(secondaryAdminDb, latestFCV);
rst.stopSet();
}());
