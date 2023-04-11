// Basic test that the two-phase commit coordinator metrics fields appear in serverStatus output.
(function() {
"use strict";

const st = new ShardingTest({shards: 1});

const res = assert.commandWorked(st.shard0.adminCommand({serverStatus: 1}));
assert.neq(null, res.twoPhaseCommitCoordinator);
assert.hasFields(res.twoPhaseCommitCoordinator, ["totalCreated"]);
assert.hasFields(res.twoPhaseCommitCoordinator, ["totalStartedTwoPhaseCommit"]);
assert.hasFields(res.twoPhaseCommitCoordinator, ["totalCommittedTwoPhaseCommit"]);
assert.hasFields(res.twoPhaseCommitCoordinator, ["totalAbortedTwoPhaseCommit"]);
assert.neq(null, res.twoPhaseCommitCoordinator.currentInSteps);
assert.hasFields(res.twoPhaseCommitCoordinator.currentInSteps, ["writingParticipantList"]);
assert.hasFields(res.twoPhaseCommitCoordinator.currentInSteps, ["waitingForVotes"]);
assert.hasFields(res.twoPhaseCommitCoordinator.currentInSteps, ["writingDecision"]);
assert.hasFields(res.twoPhaseCommitCoordinator.currentInSteps, ["waitingForDecisionAcks"]);
assert.hasFields(res.twoPhaseCommitCoordinator.currentInSteps, ["deletingCoordinatorDoc"]);

st.stop();
})();
