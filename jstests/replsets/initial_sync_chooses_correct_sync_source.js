/**
 * Tests that initial sync chooses the correct sync source based on chaining and the
 * initialSyncReadPreference.
 *
 * TODO(SERVER-46592): This test is multiversion-incompatible in 4.6.  If we use 'requires_fcv_46'
 *                     as the tag for that, removing 'requires_fcv_44' is sufficient.  Otherwise,
 *                     please set the appropriate tag when removing 'requires_fcv_44'
 * @tags: [requires_fcv_44, requires_fcv_46]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const delayMillis = 50;  // Adds a delay long enough to make a node not the "nearest" sync source.
const testName = "initial_sync_chooses_correct_sync_source";
const rst = new ReplSetTest(
    {name: testName, nodes: [{}, {rsConfig: {priority: 0}}], allowChaining: true, useBridge: true});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
const secondary = rst.getSecondary();

// Skip validation while bringing the initial sync node up and down, because we don't wait for the
// sync to complete.
TestData.skipCollectionAndIndexValidation = true;

// Add some data to be cloned.
assert.commandWorked(primaryDb.test.insert([{a: 1}, {b: 2}, {c: 3}]));
rst.awaitReplication();

jsTestLog("Testing chaining enabled, default initialSyncSourceReadPreference, non-voting node");
// Ensure we see the sync source progress messages.
TestData.setParameters = TestData.setParameters || {};
TestData.setParameters.logComponentVerbosity = TestData.setParameters.logComponentVerbosity || {};
TestData.setParameters.logComponentVerbosity.replication =
    TestData.setParameters.logComponentVerbosity.replication || {};
TestData.setParameters.logComponentVerbosity.replication =
    Object.merge(TestData.setParameters.logComponentVerbosity.replication, {verbosity: 2});
const initialSyncNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeCreatingOplog': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1
    }
});
primary.delayMessagesFrom(initialSyncNode, delayMillis);
rst.reInitiate();

assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCreatingOplog",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));
let res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));

// With zero votes and a default initialSyncSourceReadPreference, the secondary should be the sync
// source because this is equivalent to "nearest" and the primary is delayed.
assert.eq(res.syncSourceHost, secondary.host);
initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCreatingOplog", mode: "off"});

/*-----------------------------------------------------------------------------------------------*/
jsTestLog(
    "Testing chaining enabled, 'primaryPreferred' initialSyncSourceReadPreference, non-voting node");
rst.restart(initialSyncNode, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangBeforeCreatingOplog': tojson({mode: 'alwaysOn'}),
        'initialSyncSourceReadPreference': 'primaryPreferred',
        'numInitialSyncAttempts': 1
    }
});

assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCreatingOplog",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));
res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));

// With an initialSyncSourceReadPreference of 'primaryPreferred', the primary should be the sync
// source even when it is delayed.
assert.eq(res.syncSourceHost, primary.host);
initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCreatingOplog", mode: "off"});

/*-----------------------------------------------------------------------------------------------*/
/* Switch to a configuration with a voting node, which should prefer the primary normally.       */
/*-----------------------------------------------------------------------------------------------*/
jsTestLog("Reconfiguring set so initial sync node is voting.");
let config = rst.getReplSetConfigFromNode();
config.members[2].votes = 1;
config.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

jsTestLog("Testing chaining enabled, default initialSyncSourceReadPreference, voting node");
// Ensure sync source selection is logged.
rst.restart(initialSyncNode, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangBeforeCreatingOplog': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1
    }
});

assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCreatingOplog",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));
res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));

// With a voting node and a default initialSyncSourceReadPreference, the primary should be the sync
// source, though it is delayed.
assert.eq(res.syncSourceHost, primary.host);
initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCreatingOplog", mode: "off"});

/*-----------------------------------------------------------------------------------------------*/
jsTestLog(
    "Testing chaining enabled, 'secondaryPreferred' initialSyncSourceReadPreference, voting node");
rst.restart(initialSyncNode, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangBeforeCreatingOplog': tojson({mode: 'alwaysOn'}),
        'initialSyncSourceReadPreference': 'secondaryPreferred',
        'numInitialSyncAttempts': 1
    }
});

assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCreatingOplog",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));
res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));

// Even with a voting node, the secondary should be chosen when 'secondaryPreferred' is used.
assert.eq(res.syncSourceHost, secondary.host);
initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCreatingOplog", mode: "off"});

/*-----------------------------------------------------------------------------------------------*/
/* Switch back to a non-voting node, and disable chaining also.                                  */
/*-----------------------------------------------------------------------------------------------*/
jsTestLog("Reconfiguring set so initial sync node is non-voting and chaining is disabled.");
config.settings.chainingAllowed = false;
config.members[2].votes = 0;
config.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

/*-----------------------------------------------------------------------------------------------*/
jsTestLog("Testing chaining disabled, default initialSyncSourceReadPreference, non-voting node");
rst.restart(initialSyncNode, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangBeforeCreatingOplog': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1
    }
});

assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCreatingOplog",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));
res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));

// With chaining disabled, the default should be to select the primary even though it is delayed.
assert.eq(res.syncSourceHost, primary.host);
initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCreatingOplog", mode: "off"});

/*-----------------------------------------------------------------------------------------------*/
jsTestLog("Testing chaining disabled, 'nearest' initialSyncSourceReadPreference, non-voting node");
rst.restart(initialSyncNode, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangBeforeCreatingOplog': tojson({mode: 'alwaysOn'}),
        'initialSyncSourceReadPreference': 'nearest',
        'numInitialSyncAttempts': 1
    }
});

assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCreatingOplog",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));
res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));

// With chaining disabled, we choose the delayed secondary over the non-delayed primary when
// readPreference is explicitly 'nearest'.
assert.eq(res.syncSourceHost, secondary.host);
initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCreatingOplog", mode: "off"});

// Once we become secondary, the secondary read preference no longer matters and we choose the
// primary because chaining is disallowed.
assert.soon(function() {
    let res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));
    return res.syncSourceHost == primary.host;
});

primary.delayMessagesFrom(initialSyncNode, 0);
TestData.skipCollectionAndIndexValidation = false;
rst.stopSet();
})();
