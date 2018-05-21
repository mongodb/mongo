/**
 * Test that we wait for oplog visibility before beginning initial sync.
 */
(function() {
    'use strict';
    load("jstests/libs/check_log.js");

    var name = 'initial_sync_oplog_visibility';

    var replTest = new ReplSetTest({name: name, nodes: 1});
    replTest.startSet();
    replTest.initiate();
    var primary = replTest.getPrimary();

    var firstColl = "hangColl";
    var secondColl = "secondColl";

    // Create both collections.
    assert.writeOK(primary.getDB(name)[firstColl].insert({init: 1}));
    assert.writeOK(primary.getDB(name)[secondColl].insert({init: 1}));

    jsTestLog("Add a node to initial sync.");
    var secondary = replTest.add({});
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: 'initialSyncHangBeforeOplogVisibility', mode: 'alwaysOn'}));
    replTest.reInitiate();
    checkLog.contains(secondary,
                      "initial sync - initialSyncHangBeforeOplogVisibility fail point enabled");

    // Start an insert that will hang in a parallel shell.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'hangOnInsertObserver', mode: 'alwaysOn'}));
    const awaitInsertShell = startParallelShell(function() {
        var name = 'initial_sync_oplog_visibility';
        var firstColl = "hangColl";
        assert.writeOK(db.getSiblingDB(name)[firstColl].insert({a: 1}));
    }, primary.port);
    checkLog.contains(primary, "op observer - hangOnInsertObserver fail point enabled");

    // Let initial sync finish and fail.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: 'initialSyncHangBeforeOplogVisibility', mode: 'off'}));
    checkLog.contains(secondary, "initial sync attempt failed");

    // Let the insert and the initial sync finish.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'hangOnInsertObserver', mode: 'off'}));
    awaitInsertShell();
    replTest.awaitSecondaryNodes();

    replTest.stopSet();
})();