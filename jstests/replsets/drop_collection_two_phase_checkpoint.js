/**
 * Test that when enableMajorityReadConcern=false, the second phase happens when
 * the dropOptime is both majority committed and checkpointed.
 *
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/two_phase_drops.js");

    // Set syncdelay to 1hr so it never takes a checkpoint unless we manually do
    let replSet = new ReplSetTest(
        {nodes: 1, nodeOptions: {syncdelay: 60 * 60, enableMajorityReadConcern: "false"}});
    replSet.startSet();
    replSet.initiate();

    let primary = replSet.getPrimary();
    let primaryDb = primary.getDB("test");

    assert.commandWorked(primaryDb.runCommand({create: "a"}));
    assert.commandWorked(primaryDb.runCommand({create: "b"}));
    assert.commandWorked(primaryDb.runCommand({insert: "a", documents: [{x: 1}]}));

    // Take a checkpoint manually.
    assert.commandWorked(primary.getDB("admin").runCommand({fsync: 1}));

    assert.commandWorked(primaryDb.runCommand({drop: "a"}));

    // Do a write to make sure majority committed point moves.
    assert.commandWorked(primaryDb.runCommand({insert: "b", documents: [{x: 2}]}));

    assert(TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(primaryDb, "a"));

    // Take a checkpoint manually and this should move the checkpoint timestamp past dropOptime.
    assert.commandWorked(primary.getDB("admin").runCommand({fsync: 1}));

    // Do another write to move the majority committed point so that reaper is triggered.
    assert.commandWorked(primaryDb.runCommand({insert: "b", documents: [{x: 3}]}));

    TwoPhaseDropCollectionTest.waitForDropToComplete(primaryDb, "a");

    replSet.stopSet();
}());
