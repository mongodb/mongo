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
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, logComponentVerbosity: {query: 3}}));

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

    // The oplog visibility query should use an oplog-optimized plan. Check vaguely for this by
    // awaiting a characteristic log message for each storage engine - we at least know that *some*
    // query used the optimal plan around the time of the visibility query.
    const timeoutSeconds = 30;
    if (primary.adminCommand("serverStatus").storageEngine.name === "wiredTiger") {
        jsTestLog("Checking for log message about 'direct oplog seek' query plan.");
        checkLog.contains(primary, "Using direct oplog seek", timeoutSeconds);
    } else if (primary.adminCommand("serverStatus").storageEngine.name === "mmapv1") {
        jsTestLog("Checking for log message about 'Using OplogStart stage'.");
        checkLog.contains(primary, "Using OplogStart stage", timeoutSeconds);
    }

    replTest.stopSet();
})();