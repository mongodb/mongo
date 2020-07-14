/*
 * Tests the following scenarios.
 * 1. Replica set config containing 'newlyAdded' members should make fcv downgrade to fail.
 * 2. FCV downgrade blocks after a new config got mutated with 'newlyAdded' field (due to
 * addition of voters) until the mutated new config gets stored locally.
 * 3. FCV downgrade blocks until all nodes in the replica set have latest config without
 * 'newlyAdded' field.
 * 4. No 'newlyAdded' members in replica set config on fcv downgrade.
 *
 * This tests behavior centered around downgrading FCV.
 * @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

load('jstests/replsets/rslib.js');
load("jstests/libs/parallel_shell_helpers.js");  // for funWithArgs()

// Start a single node replica set.
// Disable Chaining so that initial sync nodes always sync from primary.
const rst = new ReplSetTest({nodes: 1, settings: {chainingAllowed: false}});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const dbName = jsTest.name();
const collName = "coll";

const primary = rst.getPrimary();
const db = primary.getDB(dbName);
const primaryColl = db[collName];
const primaryAdminDB = primary.getDB("admin");

function testCleanup(conn) {
    jsTestLog("Perform test cleanup");
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "initialSyncHangAfterDataCloning", mode: 'off'}));

    // Wait for the new node to be no longer newly added.
    waitForNewlyAddedRemovalForNodeToBeCommitted(primary, rst.getNodeId(conn));
    rst.waitForState(conn, ReplSetTest.State.SECONDARY);

    // Insert a doc and wait for it to replicate to all nodes.
    assert.commandWorked(primaryColl.insert({x: "somedoc"}));
    rst.awaitReplication();

    // Clear the RAM logs.
    assert.commandWorked(primary.adminCommand({clearLog: "global"}));
}

function checkFCV({version, targetVersion}) {
    assert.eq(primaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              version);
    assert.eq(
        primaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).targetVersion,
        targetVersion);
}

function addNewVotingNode({parallel: parallel = false, startupParams: startupParams = {}}) {
    const conn = rst.add({rsConfig: {priority: 0, votes: 1}, setParameter: startupParams});

    jsTestLog("Adding a new voting node {" + conn.host + "} to the replica set");
    let newConfig = rst.getReplSetConfigFromNode();
    newConfig.members = rst.getReplSetConfig().members;
    newConfig.version += 1;
    var reInitiate = (newConfig) => {
        assert.adminCommandWorkedAllowingNetworkError(
            db, {replSetReconfig: newConfig, maxTimeMS: ReplSetTest.kDefaultTimeoutMS});
    };

    const reconfigThread = parallel
        ? startParallelShell(funWithArgs(reInitiate, newConfig), primary.port)
        : reInitiate(newConfig);

    return {conn, reconfigThread};
}

function waitForInitialSyncHang(conn) {
    jsTestLog("Wait for " + conn.host + " to hang during initial sync");
    checkLog.containsJson(conn, 21184);
}

// Scenario # 1: Test 'newlyAdded' members in repl config makes fcv downgrade fail.
let newNode = addNewVotingNode(
    {startupParams: {"failpoint.initialSyncHangAfterDataCloning": tojson({mode: 'alwaysOn'})}});

waitForInitialSyncHang(newNode.conn);

// Check that 'newlyAdded' field is set.
assert(isMemberNewlyAdded(primary, 1));
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 2,
});

jsTestLog("Downgrade FCV to " + lastLTSFCV);
assert.commandFailedWithCode(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                             ErrorCodes.ConflictingOperationInProgress);

checkFCV({version: latestFCV, targetVersion: null});

// Cleanup the test.
testCleanup(newNode.conn);

// Scenario # 2: FCV downgrade blocks after a new config got mutated with 'newlyAdded' field
// (due to addition of voters) until the mutated new config gets stored locally.
//
// Make reconfig cmd to hang.
assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "ReconfigHangBeforeConfigValidationCheck", mode: 'alwaysOn'}));

// Start reconfig command in parallel shell.
newNode = addNewVotingNode({
    parallel: true,
    startupParams: {"failpoint.initialSyncHangAfterDataCloning": tojson({mode: 'alwaysOn'})}
});

jsTestLog("Wait for reconfig command on primary to hang before storing the new config locally.");
checkLog.containsJson(primary, 4637900);

let fcvDowngradeThread = startParallelShell(() => {
    jsTestLog("Downgrade FCV to " + lastLTSFCV);
    assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.ConflictingOperationInProgress);
}, primary.port);

jsTestLog("Wait for 'setFeatureCompatibilityVersion' cmd to hang on fcv resource mutex lock");
assert.soon(
    () => {
        return primaryAdminDB
                   .currentOp({
                       "command.setFeatureCompatibilityVersion": lastLTSFCV,
                       waitingForLock: true,
                       "lockStats.Mutex.acquireWaitCount.W": NumberLong(1)
                   })
                   .inprog.length === 1;
    },
    () => {
        return "Failed to find a matching op" + tojson(primaryAdminDB.currentOp().toArray());
    });

jsTestLog("Resume reconfig to unblock fcv command.");
assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "ReconfigHangBeforeConfigValidationCheck", mode: 'off'}));

waitForInitialSyncHang(newNode.conn);

// Check that 'newlyAdded' field is set.
assert(isMemberNewlyAdded(primary, 2));
assertVoteCount(primary, {
    votingMembersCount: 2,
    majorityVoteCount: 2,
    writableVotingMembersCount: 2,
    writeMajorityCount: 2,
    totalMembersCount: 3,
});

// Wait for threads to join and cleanup the test.
fcvDowngradeThread();
newNode.reconfigThread();

checkFCV({version: latestFCV, targetVersion: null});

// Cleanup the test.
testCleanup(newNode.conn);

// Scenario # 3: FCV downgrade blocks until all nodes in the replica set have latest config without
// 'newlyAdded' field.
const secondary = rst.getSecondary();
// Enable fail point on secondary0 to block secondary0 from receiving new config via heartbeat.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "blockHeartbeatReconfigFinish", mode: 'alwaysOn'}));

newNode = addNewVotingNode({});

// Wait until primary removed the 'newlyAdded' field from repl config.
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, rst.getNodeId(newNode));

// Check that 'newlyAdded' field is not set.
assert(!isMemberNewlyAdded(primary, 3));
assertVoteCount(primary, {
    votingMembersCount: 4,
    majorityVoteCount: 3,
    writableVotingMembersCount: 4,
    writeMajorityCount: 3,
    totalMembersCount: 4,
});

// FCV downgrade should fail as secondary0 config version is not up-to-date with primary's config
// version.
jsTestLog("Downgrade FCV to " + lastLTSFCV);
const res = assert.commandFailedWithCode(
    primary.adminCommand(
        {setFeatureCompatibilityVersion: lastLTSFCV, "writeConcern": {wtimeout: 5 * 1000}}),
    ErrorCodes.WriteConcernFailed);
assert(res.errmsg.startsWith(
           "Failed to wait for the current replica set config to propagate to all nodes"),
       res.errmsg);

checkFCV({version: latestFCV, targetVersion: null});

assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "blockHeartbeatReconfigFinish", mode: 'off'}));
// Cleanup the test.
testCleanup(newNode.conn);

// Scenario # 4: Test that no 'newlyAdded' members in repl config on fcv downgrade.
jsTestLog("Downgrade FCV to " + lastLTSFCV);
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

checkFCV({version: lastLTSFCV, targetVersion: null});

// Check that the "newlyAdded" field doesn't exist in the config document on all nodes.
rst.nodes.forEach(function(node) {
    jsTestLog("Checking the 'newlyAdded' field on node " + tojson(node.host) +
              " after FCV downgrade.");
    assert(!replConfigHasNewlyAddedMembers(node), () => {
        return "replSetconfig contains 'newlyAdded' field: " +
            tojson(node.getDB("local").system.replset.findOne());
    });
});

newNode = addNewVotingNode(
    {startupParams: {"failpoint.initialSyncHangAfterDataCloning": tojson({mode: 'alwaysOn'})}});

waitForInitialSyncHang(newNode.conn);

// Check that 'newlyAdded' field is not set.
assert(!isMemberNewlyAdded(primary, 4));
assertVoteCount(primary, {
    votingMembersCount: 5,
    majorityVoteCount: 3,
    writableVotingMembersCount: 5,
    writeMajorityCount: 3,
    totalMembersCount: 5,
});

// Cleanup the test.
testCleanup(newNode.conn);

rst.stopSet();
}());