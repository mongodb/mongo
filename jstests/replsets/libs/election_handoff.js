"use strict";

/**
 * This file is used for testing election handoff.
 */

var ElectionHandoffTest = (function() {

    load("jstests/libs/check_log.js");
    load("jstests/replsets/rslib.js");

    const kStepDownPeriodSecs = 30;

    /**
     * Exercises and validates an election handoff scenario by stepping down the primary and
     * ensuring that the node at "expectedCandidateId" is stepped up in its place. The desired
     * configuration of the replica set is passed in as its ReplSetTest instance.
     */
    function testElectionHandoff(rst, initialPrimaryId, expectedCandidateId) {
        const config = rst.getReplSetConfigFromNode();
        const numNodes = config.members.length;
        const memberInfo = config.members[expectedCandidateId];

        assert.neq(
            true, memberInfo["arbiterOnly"], "Election handoff candidate cannot be an arbiter.");
        assert.neq(
            0, memberInfo["priority"], "Election handoff candidate cannot have zero priority");

        rst.awaitNodesAgreeOnPrimary();
        const primary = rst.getPrimary();
        assert.eq(rst.nodes[initialPrimaryId], primary);

        // Store the term for future verification.
        const status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        const term = +status.term;

        jsTestLog("Enabling election handoff...");

        // Enable election handoff.
        assert.commandWorked(primary.adminCommand({setParameter: 1, handOffElectionOnStepdown: 1}));

        jsTestLog("Stepping down primary...");

        // Step down the current primary.
        assert.adminCommandWorkedAllowingNetworkError(primary, {
            replSetStepDown: kStepDownPeriodSecs,
            secondaryCatchUpPeriodSecs: kStepDownPeriodSecs / 2
        });

        jsTestLog(`Checking that the secondary with id ${expectedCandidateId} is stepped up...`);

        const expectedCandidate = rst.nodes[expectedCandidateId];

        // The checkLog() function blocks until the log line appears.
        checkLog.contains(expectedCandidate, "Starting an election due to step up request");

        // If there are only two nodes in the set, verify that the old primary voted "yes".
        if (numNodes === 2) {
            checkLog.contains(
                expectedCandidate,
                `VoteRequester(term ${term} dry run) received a yes vote from ${primary.host}`);
            checkLog.contains(
                expectedCandidate,
                `VoteRequester(term ${term+1}) received a yes vote from ${primary.host}`);
        }

        rst.awaitNodesAgreeOnPrimary();
        assert.eq(rst.nodes[expectedCandidateId], rst.getPrimary());
    }

    return {testElectionHandoff: testElectionHandoff, stepDownPeriodSecs: kStepDownPeriodSecs};

})();