/**
 * This test checks that the metrics around election candidates and participants are set and updated
 * correctly. We test this with a two node replica set by forcing multiple election handoffs and
 * checking the 'electionCandidateMetrics' and 'electionParticipantMetrics' fields of replSetStatus
 * after each handoff.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ElectionHandoffTest} from "jstests/replsets/libs/election_handoff.js";

const testName = jsTestName();
const numNodes = 2;
const rst = ReplSetTest({name: testName, nodes: numNodes});
rst.startSet();

// Make sure there are no election timeouts firing for the duration of the test. This helps
// ensure that the test will only pass if the election handoff succeeds.
rst.initiateWithHighElectionTimeout();

const expectedElectionTimeoutMillis = 24 * 60 * 60 * 1000;

const originalPrimary = rst.getPrimary();
let originalPrimaryReplSetGetStatus =
    assert.commandWorked(originalPrimary.adminCommand({replSetGetStatus: 1}));
jsTestLog("Original primary status (1): " + tojson(originalPrimaryReplSetGetStatus));
let originalPrimaryElectionCandidateMetrics =
    originalPrimaryReplSetGetStatus.electionCandidateMetrics;

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response after
// replica set startup has all of the required fields and that they are set correctly.
assert(originalPrimaryElectionCandidateMetrics.lastElectionReason,
       () => "Response should have an 'electionCandidateMetrics.lastElectionReason' field: " +
           tojson(originalPrimaryElectionCandidateMetrics));
assert.eq(originalPrimaryElectionCandidateMetrics.lastElectionReason, "electionTimeout");
assert(originalPrimaryElectionCandidateMetrics.lastElectionDate,
       () => "Response should have an 'electionCandidateMetrics.lastElectionDate' field: " +
           tojson(originalPrimaryElectionCandidateMetrics));
assert(originalPrimaryElectionCandidateMetrics.electionTerm,
       () => "Response should have an 'electionCandidateMetrics.electionTerm' field: " +
           tojson(originalPrimaryElectionCandidateMetrics));
assert.eq(originalPrimaryElectionCandidateMetrics.electionTerm, 1);
assert(
    originalPrimaryElectionCandidateMetrics.lastCommittedOpTimeAtElection,
    () =>
        "Response should have an 'electionCandidateMetrics.lastCommittedOpTimeAtElection' field: " +
        tojson(originalPrimaryElectionCandidateMetrics));
if (FeatureFlagUtil.isPresentAndEnabled(originalPrimary, "featureFlagReduceMajorityWriteLatency")) {
    assert(
        originalPrimaryElectionCandidateMetrics.lastSeenWrittenOpTimeAtElection,
        () =>
            "Response should have an 'electionCandidateMetrics.lastSeenWrittenOpTimeAtElection' field: " +
            tojson(originalPrimaryElectionCandidateMetrics));
}
assert(originalPrimaryElectionCandidateMetrics.lastSeenOpTimeAtElection,
       () => "Response should have an 'electionCandidateMetrics.lastSeenOpTimeAtElection' field: " +
           tojson(originalPrimaryElectionCandidateMetrics));
assert(originalPrimaryElectionCandidateMetrics.numVotesNeeded,
       () => "Response should have an 'electionCandidateMetrics.numVotesNeeded' field: " +
           tojson(originalPrimaryElectionCandidateMetrics));
assert.eq(originalPrimaryElectionCandidateMetrics.numVotesNeeded, 1);
assert(originalPrimaryElectionCandidateMetrics.priorityAtElection,
       () => "Response should have an 'electionCandidateMetrics.priorityAtElection' field: " +
           tojson(originalPrimaryElectionCandidateMetrics));
assert.eq(originalPrimaryElectionCandidateMetrics.priorityAtElection, 1.0);
assert(originalPrimaryElectionCandidateMetrics.electionTimeoutMillis,
       () => "Response should have an 'electionCandidateMetrics.electionTimeoutMillis' field: " +
           tojson(originalPrimaryElectionCandidateMetrics));
// The node runs its own election before receiving the configuration, so 'electionTimeoutMillis' is
// set to the default value.
assert.eq(originalPrimaryElectionCandidateMetrics.electionTimeoutMillis, 10000);
assert(!originalPrimaryElectionCandidateMetrics.priorPrimaryMemberId,
       () => "Response should not have an 'electionCandidateMetrics.priorPrimaryMemberId' field: " +
           tojson(originalPrimaryElectionCandidateMetrics));

ElectionHandoffTest.testElectionHandoff(rst, 0, 1);

const newPrimary = rst.getPrimary();
let newPrimaryReplSetGetStatus =
    assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
jsTestLog("New primary status (1): " + tojson(newPrimaryReplSetGetStatus));
let newPrimaryElectionCandidateMetrics = newPrimaryReplSetGetStatus.electionCandidateMetrics;

// Check that the 'electionCandidateMetrics' section of the replSetGetStatus response for the new
// primary has all of the required fields and that they are set correctly.
assert(newPrimaryElectionCandidateMetrics.lastElectionReason,
       () => "Response should have an 'electionCandidateMetrics.lastElectionReason' field: " +
           tojson(newPrimaryElectionCandidateMetrics));
assert.eq(newPrimaryElectionCandidateMetrics.lastElectionReason, "stepUpRequestSkipDryRun");
assert(newPrimaryElectionCandidateMetrics.lastElectionDate,
       () => "Response should have an 'electionCandidateMetrics.lastElectionDate' field: " +
           tojson(newPrimaryElectionCandidateMetrics));
assert(newPrimaryElectionCandidateMetrics.electionTerm,
       () => "Response should have an 'electionCandidateMetrics.electionTerm' field: " +
           tojson(newPrimaryElectionCandidateMetrics));
assert.eq(newPrimaryElectionCandidateMetrics.electionTerm, 2);
assert(
    newPrimaryElectionCandidateMetrics.lastCommittedOpTimeAtElection,
    () =>
        "Response should have an 'electionCandidateMetrics.lastCommittedOpTimeAtElection' field: " +
        tojson(newPrimaryElectionCandidateMetrics));
assert(newPrimaryElectionCandidateMetrics.lastSeenOpTimeAtElection,
       () => "Response should have an 'electionCandidateMetrics.lastSeenOpTimeAtElection' field: " +
           tojson(newPrimaryElectionCandidateMetrics));
assert(newPrimaryElectionCandidateMetrics.numVotesNeeded,
       () => "Response should have an 'electionCandidateMetrics.numVotesNeeded' field: " +
           tojson(newPrimaryElectionCandidateMetrics));
assert.eq(newPrimaryElectionCandidateMetrics.numVotesNeeded, 2);
assert(newPrimaryElectionCandidateMetrics.priorityAtElection,
       () => "Response should have an 'electionCandidateMetrics.priorityAtElection' field: " +
           tojson(newPrimaryElectionCandidateMetrics));
assert.eq(newPrimaryElectionCandidateMetrics.priorityAtElection, 1.0);
assert(newPrimaryElectionCandidateMetrics.electionTimeoutMillis,
       () => "Response should have an 'electionCandidateMetrics.electionTimeoutMillis' field: " +
           tojson(newPrimaryElectionCandidateMetrics));
assert.eq(newPrimaryElectionCandidateMetrics.electionTimeoutMillis, expectedElectionTimeoutMillis);
// Since the previous primary's ID is 0, we directly assert that 0 is stored in the
// priorPrimaryMemberId field.
// TODO (SERVER-45274): Re-enable this assertion.
// assert.eq(newPrimaryElectionCandidateMetrics.priorPrimaryMemberId, 0);

let newPrimaryElectionParticipantMetrics = newPrimaryReplSetGetStatus.electionParticipantMetrics;

// The new primary should not have its 'electionParticipantMetrics' field set, since it was the
// candidate in this election and did not vote for any other node.
assert(!newPrimaryElectionParticipantMetrics,
       () => "Response should not have an 'electionParticipantMetric' field: " +
           tojson(newPrimaryReplSetGetStatus));

originalPrimaryReplSetGetStatus =
    assert.commandWorked(originalPrimary.adminCommand({replSetGetStatus: 1}));
jsTestLog("Original primary status (2): " + tojson(originalPrimaryReplSetGetStatus));
let originalPrimaryElectionParticipantMetrics =
    originalPrimaryReplSetGetStatus.electionParticipantMetrics;

// Check that the 'electionParticipantMetrics' section of the replSetGetStatus response for the
// original primary has all of the required fields and that they are set correctly.
assert(originalPrimaryElectionParticipantMetrics.votedForCandidate,
       () => "Response should have an 'electionParticipantMetrics.votedForCandidate' field: " +
           tojson(originalPrimaryElectionParticipantMetrics));
assert.eq(originalPrimaryElectionParticipantMetrics.votedForCandidate, true);
assert(originalPrimaryElectionParticipantMetrics.electionTerm,
       () => "Response should have an 'electionParticipantMetrics.electionTerm' field: " +
           tojson(originalPrimaryElectionParticipantMetrics));
assert.eq(originalPrimaryElectionParticipantMetrics.electionTerm, 2);
assert(originalPrimaryElectionParticipantMetrics.lastVoteDate,
       () => "Response should have an 'electionParticipantMetrics.lastVoteDate' field: " +
           tojson(originalPrimaryElectionParticipantMetrics));
assert(
    originalPrimaryElectionParticipantMetrics.electionCandidateMemberId,
    () => "Response should have an 'electionParticipantMetrics.electionCandidateMemberId' field: " +
        tojson(originalPrimaryElectionParticipantMetrics));
assert.eq(originalPrimaryElectionParticipantMetrics.electionCandidateMemberId, 1);
// Since the node voted for the new primary, we directly assert that its voteReason is equal to
// empty string.
assert.eq(originalPrimaryElectionParticipantMetrics.voteReason, "");
if (FeatureFlagUtil.isPresentAndEnabled(originalPrimary, "featureFlagReduceMajorityWriteLatency")) {
    assert(
        originalPrimaryElectionParticipantMetrics.lastWrittenOpTimeAtElection,
        () =>
            "Response should have an 'electionParticipantMetrics.lastWrittenOpTimeAtElection' field: " +
            tojson(originalPrimaryElectionParticipantMetrics));
    assert(
        originalPrimaryElectionParticipantMetrics.maxWrittenOpTimeInSet,
        () => "Response should have an 'electionParticipantMetrics.maxWrittenOpTimeInSet' field: " +
            tojson(originalPrimaryElectionParticipantMetrics));
}
assert(
    originalPrimaryElectionParticipantMetrics.lastAppliedOpTimeAtElection,
    () =>
        "Response should have an 'electionParticipantMetrics.lastAppliedOpTimeAtElection' field: " +
        tojson(originalPrimaryElectionParticipantMetrics));
assert(originalPrimaryElectionParticipantMetrics.maxAppliedOpTimeInSet,
       () => "Response should have an 'electionParticipantMetrics.maxAppliedOpTimeInSet' field: " +
           tojson(originalPrimaryElectionParticipantMetrics));
assert(originalPrimaryElectionParticipantMetrics.priorityAtElection,
       () => "Response should have an 'electionParticipantMetrics.priorityAtElection' field: " +
           tojson(originalPrimaryElectionParticipantMetrics));
assert.eq(originalPrimaryElectionParticipantMetrics.priorityAtElection, 1);

originalPrimaryElectionCandidateMetrics = originalPrimaryReplSetGetStatus.electionCandidateMetrics;

// The original primary should not have its 'electionCandidateMetrics' field set, since it was not a
// candidate in this election.
assert(!originalPrimaryElectionCandidateMetrics,
       () => "Response should not have an 'electionCandidateMetrics' field: " +
           tojson(originalPrimaryElectionCandidateMetrics));

// testElectionHandoff steps down the primary with a non-zero step down period, so we need to
// unfreeze the node to allow it to initiate an election again.
assert.commandWorked(originalPrimary.adminCommand({replSetFreeze: 0}));
// Step up the original primary.
ElectionHandoffTest.testElectionHandoff(rst, 1, 0);

originalPrimaryReplSetGetStatus =
    assert.commandWorked(originalPrimary.adminCommand({replSetGetStatus: 1}));
jsTestLog("Original primary status (3): " + tojson(originalPrimaryReplSetGetStatus));
originalPrimaryElectionCandidateMetrics = originalPrimaryReplSetGetStatus.electionCandidateMetrics;

// Check that the original primary's metrics are also being set properly after the second election.
assert.eq(originalPrimaryElectionCandidateMetrics.lastElectionReason, "stepUpRequestSkipDryRun");
assert.eq(originalPrimaryElectionCandidateMetrics.electionTerm, 3);
assert.eq(originalPrimaryElectionCandidateMetrics.numVotesNeeded, 2);
assert.eq(originalPrimaryElectionCandidateMetrics.priorityAtElection, 1);
assert.eq(originalPrimaryElectionCandidateMetrics.electionTimeoutMillis,
          expectedElectionTimeoutMillis);
// TODO (SERVER-45274): Re-enable this assertion.
// assert.eq(originalPrimaryElectionCandidateMetrics.priorPrimaryMemberId, 1);

newPrimaryReplSetGetStatus = assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
jsTestLog("New primary status (2): " + tojson(newPrimaryReplSetGetStatus));
newPrimaryElectionCandidateMetrics = newPrimaryReplSetGetStatus.electionCandidateMetrics;
newPrimaryElectionParticipantMetrics = newPrimaryReplSetGetStatus.electionParticipantMetrics;

// The other node should not have an electionCandidateMetrics, as it just stepped down.
assert(!newPrimaryElectionCandidateMetrics,
       () => "Response should not have an 'electionCandidateMetrics' field: " +
           tojson(newPrimaryReplSetGetStatus));

// Check that the primary that just stepped down has its 'electionParticipantMetrics' field set
// correctly.
assert.eq(newPrimaryElectionParticipantMetrics.votedForCandidate, true);
assert.eq(newPrimaryElectionParticipantMetrics.electionTerm, 3);
assert.eq(newPrimaryElectionParticipantMetrics.electionCandidateMemberId, 0);
assert.eq(newPrimaryElectionParticipantMetrics.voteReason, "");
assert.eq(newPrimaryElectionParticipantMetrics.priorityAtElection, 1);

// Since the election participant metrics are only set in the real election, set up a failpoint that
// tells a voting node to vote yes in the dry run election and no in the real election.
assert.commandWorked(originalPrimary.adminCommand(
    {configureFailPoint: "voteYesInDryRunButNoInRealElection", mode: "alwaysOn"}));

// The new primary might still be processing the reconfig via heartbeat from the original primary's
// reconfig on step up. Wait for config replication first so it doesn't interfere with the step up
// on the new primary below.
rst.waitForConfigReplication(originalPrimary);

// Attempt to step up the new primary a second time. Due to the failpoint, the current primary
// should vote no, and as a result the election should fail.
assert.commandWorked(newPrimary.adminCommand({replSetFreeze: 0}));
// Make sure the step up failed and for the right reason.
assert.commandFailedWithCode(newPrimary.adminCommand({replSetStepUp: 1}), ErrorCodes.CommandFailed);
assert(
    checkLog.checkContainsOnce(newPrimary, "Not becoming primary, we received insufficient votes"));

originalPrimaryReplSetGetStatus =
    assert.commandWorked(originalPrimary.adminCommand({replSetGetStatus: 1}));
jsTestLog("Original primary status (4): " + tojson(originalPrimaryReplSetGetStatus));
originalPrimaryElectionParticipantMetrics =
    originalPrimaryReplSetGetStatus.electionParticipantMetrics;

// Check that the metrics in 'electionParticipantMetrics' were updated for the original primary
// after the second election that it participated in.
assert.eq(originalPrimaryElectionParticipantMetrics.votedForCandidate, false);
assert.eq(originalPrimaryElectionParticipantMetrics.electionTerm, 4);
assert.eq(originalPrimaryElectionParticipantMetrics.electionCandidateMemberId, 1);
assert.eq(
    originalPrimaryElectionParticipantMetrics.voteReason,
    "forced to vote no in real election due to failpoint voteYesInDryRunButNoInRealElection set");
assert.eq(originalPrimaryElectionParticipantMetrics.priorityAtElection, 1);

rst.stopSet();
