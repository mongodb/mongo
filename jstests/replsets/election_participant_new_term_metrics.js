/**
 * This test checks that the 'newTermStartDate' and 'newTermAppliedDate' metrics in
 * 'electionParticipantMetrics' are set and updated correctly. We test this with a three node
 * replica set by successfully stepping up one of the secondaries, then failing to step up the
 * original primary. We check that the metrics are appropriately set or unset after each election.
 */

(function() {
"use strict";

const testName = jsTestName();
const rst = ReplSetTest({name: testName, nodes: [{}, {}, {rsConfig: {priority: 0}}]});
rst.startSet();

// Make sure there are no election timeouts firing for the duration of the test. This helps
// ensure that the test will only pass if the election succeeds.
rst.initiateWithHighElectionTimeout();

const originalPrimary = rst.getPrimary();
const newPrimary = rst.getSecondaries()[0];
const testNode = rst.getSecondaries()[1];

// Set up a failpoint that forces the original primary to vote no in this election. This guarantees
// that 'testNode' will be a participant in this election, since its vote will be needed for the new
// primary to win.
assert.commandWorked(
    originalPrimary.adminCommand({configureFailPoint: "voteNoInElection", mode: "alwaysOn"}));

// Step up the new primary.
assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
rst.awaitNodesAgreeOnPrimary();
assert.eq(newPrimary, rst.getPrimary());

// Since the new term oplog entry needs to be replicated onto testNode for the metrics to be set, we
// must await replication before checking the metrics.
rst.awaitReplication();

let testNodeReplSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));
let testNodeElectionParticipantMetrics = testNodeReplSetGetStatus.electionParticipantMetrics;

const originalNewTermStartDate = testNodeElectionParticipantMetrics.newTermStartDate;
const originalNewTermAppliedDate = testNodeElectionParticipantMetrics.newTermAppliedDate;

// These fields should be set, since testNode has received and applied the new term oplog entry
// after the election.
assert(originalNewTermStartDate,
       () => "Response should have an 'electionParticipantMetrics.newTermStartDate' field: " +
           tojson(testNodeElectionParticipantMetrics));
assert(originalNewTermAppliedDate,
       () => "Response should have an 'electionParticipantMetrics.newTermAppliedDate' field: " +
           tojson(testNodeElectionParticipantMetrics));

// Set up a failpoint that forces newPrimary and testNode to vote no in the election, in addition to
// the new primary above. This will cause the dry run to fail for the original primary.
assert.commandWorked(
    newPrimary.adminCommand({configureFailPoint: "voteNoInElection", mode: "alwaysOn"}));
assert.commandWorked(
    testNode.adminCommand({configureFailPoint: "voteNoInElection", mode: "alwaysOn"}));

// Attempt to step up the original primary.
assert.commandFailedWithCode(originalPrimary.adminCommand({replSetStepUp: 1}),
                             ErrorCodes.CommandFailed);

testNodeReplSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));
testNodeElectionParticipantMetrics = testNodeReplSetGetStatus.electionParticipantMetrics;

// The 'newTermStartDate' and 'newTermAppliedDate' fields should not be cleared, since the term is
// not incremented when a dry run election is initiated.
assert(testNodeElectionParticipantMetrics.newTermStartDate,
       () => "Response should have an 'electionParticipantMetrics.newTermStartDate' field: " +
           tojson(testNodeElectionParticipantMetrics));
assert(testNodeElectionParticipantMetrics.newTermAppliedDate,
       () => "Response should have an 'electionParticipantMetrics.newTermAppliedDate' field: " +
           tojson(testNodeElectionParticipantMetrics));

// The fields should store the same dates, since a new term oplog entry was not received.
assert.eq(originalNewTermStartDate, testNodeElectionParticipantMetrics.newTermStartDate);
assert.eq(originalNewTermAppliedDate, testNodeElectionParticipantMetrics.newTermAppliedDate);

// Clear the previous failpoint and set up a new one that forces the two current secondaries to vote
// yes for the candidate in the dry run election and no in the real election. This will cause the
// real election to fail.
assert.commandWorked(
    newPrimary.adminCommand({configureFailPoint: "voteNoInElection", mode: "off"}));
assert.commandWorked(newPrimary.adminCommand(
    {configureFailPoint: "voteYesInDryRunButNoInRealElection", mode: "alwaysOn"}));
assert.commandWorked(testNode.adminCommand({configureFailPoint: "voteNoInElection", mode: "off"}));
assert.commandWorked(testNode.adminCommand(
    {configureFailPoint: "voteYesInDryRunButNoInRealElection", mode: "alwaysOn"}));

// Attempt to step up the original primary.
assert.commandFailedWithCode(originalPrimary.adminCommand({replSetStepUp: 1}),
                             ErrorCodes.CommandFailed);

testNodeReplSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));
testNodeElectionParticipantMetrics = testNodeReplSetGetStatus.electionParticipantMetrics;

// Since the election succeeded in the dry run, a new term was encountered by the secondary.
// However, because the real election failed, there was no new term oplog entry, so these fields
// should not be set.
assert(!testNodeElectionParticipantMetrics.newTermStartDate,
       () => "Response should not have an 'electionParticipantMetrics.newTermStartDate' field: " +
           tojson(testNodeElectionParticipantMetrics));
assert(!testNodeElectionParticipantMetrics.newTermAppliedDate,
       () => "Response should not have an 'electionParticipantMetrics.newTermAppliedDate' field: " +
           tojson(testNodeElectionParticipantMetrics));

rst.stopSet();
})();
