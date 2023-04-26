/**
 * Tests that if chaining is disabled, electing a new primary will cause nodes to start syncing from
 * the new primary.
 */

(function() {
"use strict";

const replSet = new ReplSetTest({
    nodes: 3,
    settings: {chainingAllowed: false},
    // We will turn on the noop writer after newPrimary is elected to ensure that newPrimary will
    // eventually be an eligible sync source for the secondary. We don't turn it on at the start of
    // the test because the noop writer could cause newPrimary to lose the election.
    nodeOptions: {setParameter: {writePeriodicNoops: false, periodicNoopIntervalSecs: 1}}
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const oldPrimary = replSet.getPrimary();
const [newPrimary, secondary] = replSet.getSecondaries();

replSet.awaitSyncSource(secondary, oldPrimary);

replSet.stepUp(newPrimary);

// Enable periodic noops so that the secondary can sync from newPrimary.
assert.commandWorked(newPrimary.adminCommand({setParameter: 1, writePeriodicNoops: true}));

replSet.awaitSyncSource(secondary, newPrimary);

replSet.stopSet();
})();
