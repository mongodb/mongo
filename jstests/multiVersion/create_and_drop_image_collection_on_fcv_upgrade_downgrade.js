/**
 * Tests that upgrading and downgrading the FCV will create and drop the 'config.image_collection'
 * table respectively.
 */

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/feature_compatibility_version.js");

    const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    const collName = 'image_collection';
    const primaryAdminDB = primary.getDB('admin');
    const secondaryAdminDB = secondary.getDB('admin');
    const primaryConfigDB = primary.getDB('config');
    const secondaryConfigDB = secondary.getDB('config');
    // The collection exists on startup in the latest FCV.
    checkFCV(primaryAdminDB, latestFCV);
    checkFCV(secondaryAdminDB, latestFCV);
    assert(primaryConfigDB[collName].exists());
    assert(secondaryConfigDB[collName].exists());

    assert.commandWorked(
        primaryAdminDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(primaryAdminDB, lastStableFCV);
    checkFCV(secondaryAdminDB, lastStableFCV);
    assert(!primaryConfigDB[collName].exists());
    assert(!secondaryConfigDB[collName].exists());

    // Restart the primary to make sure the collection is not created on startup while in the
    // downgraded FCV.
    rst.restart(primary);
    rst.getPrimary();
    reconnect(primary);
    reconnect(secondary);

    checkFCV(primaryAdminDB, lastStableFCV);
    checkFCV(secondaryAdminDB, lastStableFCV);
    assert(!primaryConfigDB[collName].exists());
    assert(!secondaryConfigDB[collName].exists());

    assert.commandWorked(primaryAdminDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(primaryAdminDB, latestFCV);
    checkFCV(secondaryAdminDB, latestFCV);
    assert(primaryConfigDB[collName].exists());
    assert(secondaryConfigDB[collName].exists());
    rst.stopSet();
})();