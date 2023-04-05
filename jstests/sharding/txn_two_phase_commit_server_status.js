// Basic test that the two-phase commit coordinator metrics fields appear in serverStatus output.
(function() {
"use strict";

const st = new ShardingTest({shards: 1});

const res = assert.commandWorked(st.shard0.adminCommand({serverStatus: 1}));
assert.neq(null, res.twoPhaseCommitCoordinator);
// A catalog shard will have run config server metadata transactions, which are single shard but
// create a two phase commit coordinator.
assert.eq(TestData.catalogShard ? 1 : 0, res.twoPhaseCommitCoordinator.totalCreated);
assert.eq(0, res.twoPhaseCommitCoordinator.totalStartedTwoPhaseCommit);
assert.eq(0, res.twoPhaseCommitCoordinator.totalCommittedTwoPhaseCommit);
assert.eq(0, res.twoPhaseCommitCoordinator.totalAbortedTwoPhaseCommit);
assert.neq(null, res.twoPhaseCommitCoordinator.currentInSteps);
assert.eq(0, res.twoPhaseCommitCoordinator.currentInSteps.writingParticipantList);
assert.eq(0, res.twoPhaseCommitCoordinator.currentInSteps.waitingForVotes);
assert.eq(0, res.twoPhaseCommitCoordinator.currentInSteps.writingDecision);
assert.eq(0, res.twoPhaseCommitCoordinator.currentInSteps.waitingForDecisionAcks);
assert.eq(0, res.twoPhaseCommitCoordinator.currentInSteps.deletingCoordinatorDoc);

st.stop();
})();
