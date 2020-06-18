load('jstests/libs/sessions_collection.js');

(function() {
"use strict";

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

var replTest = new ReplSetTest({
    name: 'refresh',
    nodes: [
        {/* primary */},
        {/* secondary */ rsConfig: {priority: 0}},
        {/* arbiter */ rsConfig: {arbiterOnly: true}}
    ]
});
var nodes = replTest.startSet();

replTest.initiate();
var primary = replTest.getPrimary();
var primaryAdmin = primary.getDB("admin");

replTest.awaitSecondaryNodes();
var secondary = replTest.getSecondary();
var secondaryAdmin = secondary.getDB("admin");

let arbiter = replTest.getArbiter();

const refreshErrorMsgRegex =
    new RegExp("Failed to refresh session cache, will try again at the next refresh interval");

// Get the current value of the TTL index so that we can verify it's being properly applied.
let res = assert.commandWorked(
    primary.adminCommand({getParameter: 1, localLogicalSessionTimeoutMinutes: 1}));
let timeoutMinutes = res.localLogicalSessionTimeoutMinutes;

// Test that we can use sessions on the primary before the sessions collection exists.
{
    validateSessionsCollection(primary, false, false, timeoutMinutes);

    assert.commandWorked(primaryAdmin.runCommand({startSession: 1}));

    validateSessionsCollection(primary, false, false, timeoutMinutes);
}

// Test that we can use sessions on secondaries before the sessions collection exists.
{
    validateSessionsCollection(primary, false, false, timeoutMinutes);

    replTest.awaitReplication();
    validateSessionsCollection(secondary, false, false, timeoutMinutes);

    assert.commandWorked(secondaryAdmin.runCommand({startSession: 1}));

    validateSessionsCollection(primary, false, false, timeoutMinutes);

    replTest.awaitReplication();
    validateSessionsCollection(secondary, false, false, timeoutMinutes);
}

// Test that a refresh on a secondary does not create the sessions collection.
{
    validateSessionsCollection(primary, false, false, timeoutMinutes);

    replTest.awaitReplication();
    validateSessionsCollection(secondary, false, false, timeoutMinutes);

    assert.commandWorked(secondaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

    validateSessionsCollection(primary, false, false, timeoutMinutes);

    replTest.awaitReplication();
    validateSessionsCollection(secondary, false, false, timeoutMinutes);
}

// Test that a refresh on an arbiter does not create the sessions collection.
{
    validateSessionsCollection(primary, false, false, timeoutMinutes);

    assert.commandWorked(arbiter.adminCommand({clearLog: 'global'}));
    assert.commandWorked(arbiter.adminCommand({refreshLogicalSessionCacheNow: 1}));

    validateSessionsCollection(primary, false, false, timeoutMinutes);

    if (!jsTest.options().useRandomBinVersionsWithinReplicaSet) {
        // Verify that the arbiter did not try to set up the session collection or refresh.
        assert.eq(false, checkLog.checkContainsOnce(arbiter, refreshErrorMsgRegex));
    }
}

// Test that a refresh on the primary creates the sessions collection.
{
    validateSessionsCollection(primary, false, false, timeoutMinutes);

    replTest.awaitReplication();
    validateSessionsCollection(secondary, false, false, timeoutMinutes);

    assert.commandWorked(primaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

    validateSessionsCollection(primary, true, true, timeoutMinutes);
}

// Test that a refresh on a secondary will not create the TTL index on the sessions collection.
{
    assert.commandWorked(primary.getDB("config").system.sessions.dropIndex({lastUse: 1}));

    validateSessionsCollection(primary, true, false, timeoutMinutes);

    assert.commandWorked(secondaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

    validateSessionsCollection(primary, true, false, timeoutMinutes);
}

// Test that a refresh on an arbiter will not create the TTL index on the sessions collection.
{
    validateSessionsCollection(primary, true, false, timeoutMinutes);

    assert.commandWorked(arbiter.adminCommand({refreshLogicalSessionCacheNow: 1}));

    validateSessionsCollection(primary, true, false, timeoutMinutes);
}

// Test that a refresh on the primary will create the TTL index on the sessions collection.
{
    validateSessionsCollection(primary, true, false, timeoutMinutes);

    assert.commandWorked(primaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

    validateSessionsCollection(primary, true, true, timeoutMinutes);
}

timeoutMinutes = 4;

replTest.restart(
    0, {startClean: false, setParameter: "localLogicalSessionTimeoutMinutes=" + timeoutMinutes});

primary = replTest.getPrimary();
primaryAdmin = primary.getDB("admin");
secondary = replTest.getSecondary();

// Test that a change to the TTL index expiration on restart will generate a collMod to change
// the expiration time.
{
    assert.commandWorked(primaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));

    validateSessionsCollection(primary, true, true, timeoutMinutes);

    replTest.awaitReplication();
    validateSessionsCollection(secondary, true, true, timeoutMinutes);
}

replTest.stopSet();
})();
