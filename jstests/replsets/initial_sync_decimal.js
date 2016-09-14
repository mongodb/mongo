/**
 * Tests that we can initial sync decimal128 data to secondaries, even when the cluster is in 3.2
 * feature compatibility mode.
 */
(function() {
    "use strict";

    var replTest = new ReplSetTest({name: 'testSet', nodes: 2});

    var nodes = replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();
    var primaryDB = primary.getDB("test");
    var primaryColl = primaryDB.initial_sync_decimal;

    var secondary = replTest.getSecondary();

    // Since we started a fresh replica set, we should be in 3.4 feature compatibility mode.
    var fcv = assert.commandWorked(
        primaryDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(fcv.featureCompatibilityVersion, "3.4");

    // Make sure we can insert decimal data in 3.4 mode.
    assert.writeOK(primaryColl.insert({a: 1, b: NumberDecimal(1)}));

    // Set 3.2 feature compatibility mode.
    assert.commandWorked(primaryDB.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    fcv = assert.commandWorked(
        primaryDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(fcv.featureCompatibilityVersion, "3.2");

    // Ensure 3.2 feature compatibility version has synced to admin.system.version.
    fcv = secondary.getDB("admin")
              .system.version.find({_id: "featureCompatibilityVersion"})
              .toArray();
    assert.eq(1, fcv.length);
    assert.eq("3.2", fcv[0].version);

    // We shouldn't be able to insert decimal data in 3.2 mode.
    assert.writeError(primaryColl.insert({a: 1, b: NumberDecimal(1)}));

    // We should, however, be able to initial sync decimal data.
    assert.commandWorked(secondary.getDB("admin").runCommand({resync: 1}));
    replTest.awaitSecondaryNodes();

    // Ensure that secondary is still up and has the data.
    var secondaryColl = secondary.getDB("test").initial_sync_decimal;
    assert.eq(1, secondaryColl.find({b: NumberDecimal(1)}).itcount());

    replTest.stopSet();
})();
