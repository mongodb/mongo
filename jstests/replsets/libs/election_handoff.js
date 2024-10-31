/**
 * This file is used for testing election handoff.
 */

export var ElectionHandoffTest = (function() {
    const kStepDownPeriodSecs = 30;
    const kSIGTERM = 15;
    const kTimeoutMS = 5 * 60 * 1000;  // 5 minutes

    /**
     * Exercises and validates an election handoff scenario by stepping down the primary and
     * ensuring that the node at "expectedCandidateId" is stepped up in its place. The desired
     * configuration of the replica set is passed in as its ReplSetTest instance.
     *
     * The options parameter contains extra options for the handoff.  Currently supported options
     * are:
     *   stepDownBySignal - When this option is set, the primary will be stepped down by stopping
     *                      and restarting with sigterm, rather than with a replSetStepDown command
     *   stepDownPeriodSecs - The number of seconds to step down the primary.
     *   secondaryCatchUpPeriodSecs - The number of seconds that the mongod will wait for an
     *                                electable secondary to catch up to the primary.
     */
    function testElectionHandoff(rst, initialPrimaryId, expectedCandidateId, options = {}) {
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

        // Election handoff is enabled by default. This test explicitly configures it for safety
        // purposes.
        assert.commandWorked(primary.adminCommand({setParameter: 1, enableElectionHandoff: 1}));

        jsTestLog("Stepping down primary...");

        // Make sure all secondaries are ready before stepping down. We must additionally
        // make sure that the primary is aware that the secondaries are ready and caught up
        // to the primary's lastApplied, so we issue a dummy write and wait on its optime.
        assert.commandWorked(primary.getDB("test").secondariesMustBeCaughtUpToHere.insert(
            {"a": 1}, {writeConcern: {w: rst.nodes.length}}));
        rst.awaitNodesAgreeOnAppliedOpTime();

        // Make sure we clear any logs of RST setup, which can involve stepping up the original
        // primary, before checking for stepup logs from the expected candidate node.
        clearRawMongoProgramOutput();

        // Step down the current primary. Skip validation since it prevents election handoff.
        if (options["stepDownBySignal"]) {
            rst.stop(initialPrimaryId, kSIGTERM, {skipValidation: true}, {forRestart: true});
            rst.start(initialPrimaryId, {}, true);
        } else {
            assert.commandWorked(primary.adminCommand({
                replSetStepDown: options.stepDownPeriodSecs || kStepDownPeriodSecs,
                secondaryCatchUpPeriodSecs:
                    options.secondaryCatchUpPeriodSecs || kStepDownPeriodSecs / 2
            }));
        }

        jsTestLog(`Checking that the secondary with id ${expectedCandidateId} is stepped up...`);

        const expectedCandidate = rst.nodes[expectedCandidateId];

        let subStr = "Starting an election due to step up request";
        assert.soon(function() {
            return rawMongoProgramOutput(".*").includes(subStr);
        }, "Secondary should have stepped up via step up request", kTimeoutMS);

        // If there are only two nodes in the set, verify that the old primary voted "yes".
        if (numNodes === 2) {
            subStr = `Skipping dry run and running for election","attr":{"newTerm":${term + 1}}}`;
            assert.soon(function() {
                return rawMongoProgramOutput(".*").includes(subStr);
            }, "Secondary should have run for an election in term: " + (term + 1), kTimeoutMS);

            subStr = `{"term":${term + 1},"dryRun":false,"vote":"yes","from":"${primary.host}"`;
            jsTestLog(subStr);
            assert.soon(function() {
                return rawMongoProgramOutput(".*").includes(subStr);
            }, "Primary should have voted yes for secondary", kTimeoutMS);
        }

        rst.awaitNodesAgreeOnPrimary();
        assert.eq(rst.nodes[expectedCandidateId], rst.getPrimary());
    }

    return {testElectionHandoff: testElectionHandoff, stepDownPeriodSecs: kStepDownPeriodSecs};
})();
