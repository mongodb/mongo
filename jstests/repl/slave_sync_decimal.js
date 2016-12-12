/**
 * Tests that we can initial sync decimal128 data to a slave, even when the cluster is in 3.2
 * feature compatibility mode.
 */
(function() {
    "use strict";

    var replTest = new ReplTest("slave_sync_decimal");

    var master = replTest.start(true);
    var masterDB = master.getDB("test");
    var masterColl = masterDB.slave_sync_decimal;

    // Since the master started fresh, it should be in 3.4 feature compatibility mode.
    var fcv = assert.commandWorked(
        masterDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(fcv.featureCompatibilityVersion, "3.4");

    // Make sure we can insert decimal data in 3.4 mode.
    assert.writeOK(masterColl.insert({a: 1, b: NumberDecimal(1)}));

    // Set 3.2 feature compatibility mode.
    assert.commandWorked(masterDB.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    fcv = assert.commandWorked(
        masterDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(fcv.featureCompatibilityVersion, "3.2");

    // We shouldn't be able to insert decimal data in 3.2 mode.
    assert.writeError(masterColl.insert({a: 1, b: NumberDecimal(1)}));

    var slave = replTest.start(false);
    var slaveDB = slave.getDB("test");
    var slaveColl = slaveDB.slave_sync_decimal;

    fcv = assert.commandWorked(
        slaveDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(fcv.featureCompatibilityVersion, "3.2");

    slave.setSlaveOk(true);

    assert.soon(function() {
        return slaveColl.find({b: NumberDecimal(1)}).itcount() === 1;
    }, 5 * 60 * 1000, "Slave failed to sync document containing NumberDecimal");

    replTest.stop();
})();
