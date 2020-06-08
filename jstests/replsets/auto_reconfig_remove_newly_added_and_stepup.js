/**
 * This test sets up a node to have a 'newlyAdded' field, then lets the removal of that field race
 * with the config term bump during step up. Both automatic reconfigs must be resilient to failure
 * and are expected to retry until success.
 *
 * @tags: [
 *   requires_fcv_46,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/replsets/rslib.js');

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

const rst = new ReplSetTest(
    {name: testName, nodes: 1, settings: {chainingAllowed: false}, useBridge: true});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

assert.commandWorked(primaryColl.insert({"starting": "doc"}));
let hangBeforeNewlyAddedRemovalFP = configureFailPoint(primaryDb, "hangDuringAutomaticReconfig");

jsTestLog("Adding a new node to the replica set");
const secondary = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        'numInitialSyncAttempts': 1,
    }
});
rst.reInitiate();
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);

jsTestLog("Checking that the primary has initiated the removal of 'newlyAdded'");
hangBeforeNewlyAddedRemovalFP.wait();

jsTestLog("Stepping down primary (so we can step it back up)");
const configBeforeTermBump = assert.commandWorked(primaryDb.adminCommand({replSetGetConfig: 1}));
assert.eq(1, configBeforeTermBump.config.term, () => tojson(configBeforeTermBump));
assert.commandWorked(primaryDb.adminCommand({replSetStepDown: 10 * 60, force: true}));

let hangBeforeTermBumpFP = configureFailPoint(primaryDb, "hangBeforeRSTLOnDrainComplete");

jsTestLog("Stepping up primary");
assert.commandWorked(primaryDb.adminCommand({replSetFreeze: 0}));
assert.commandWorked(primaryDb.adminCommand({replSetStepUp: 1}));
hangBeforeTermBumpFP.wait();

jsTestLog("Releasing both failpoints");
const bumpFirst = (Math.random() > 0.5);
const sleepAmount = Math.floor(Math.random() * 1000);  // 0-1000 ms
jsTestLog("Will sleep for " + sleepAmount + " milliseconds");

if (bumpFirst) {
    jsTestLog("[1] Releasing term bump FP first");
    hangBeforeTermBumpFP.off();
    sleep(sleepAmount);
    hangBeforeNewlyAddedRemovalFP.off();
} else {
    jsTestLog("[2] Releasing 'newlyAdded' removal FP first");
    hangBeforeNewlyAddedRemovalFP.off();
    sleep(sleepAmount);
    hangBeforeTermBumpFP.off();
}

jsTestLog("Waiting for 'newlyAdded' field to be removed");
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 1);
assertVoteCount(primary, {
    votingMembersCount: 2,
    majorityVoteCount: 2,
    writableVotingMembersCount: 2,
    writeMajorityCount: 2,
    totalMembersCount: 2,
});

jsTestLog("Making sure the config term has been updated");
assert.eq(primary, rst.getPrimary());
const configAfterTermBump = assert.commandWorked(primaryDb.adminCommand({replSetGetConfig: 1}));
assert.eq(2,
          configAfterTermBump.config.term,
          () => [tojson(configBeforeTermBump), tojson(configAfterTermBump)]);

rst.stopSet();
})();