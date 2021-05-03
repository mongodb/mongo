/**
 * Asserts that a reconfig from a replica set with one writable voting node to a
 * Primary-Secondary-Arbiter (PSA) topology fails if the secondary is electable. We test two
 * reconfig scenarios, both of which should fail:
 *
 * 1) PA set to PSA set
 * 2) PSA set with S having {votes: 0, priority: 0} to S with {votes: 1, priority: 1}
 *
 * Finally, we test the correct workflow for converting a replica set with only one writable voting
 * node to a PSA architecture. This involves running two reconfigs. The first reconfig should
 * add/configure the secondary to have {votes: 1, priority: 0}, to prevent it from being electable.
 * The second reconfig should then increase its priority to the desired level.
 *
 * @tags: [requires_fcv_50]
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");

{
    jsTestLog("Testing reconfig from PA set to PSA set fails");
    const rst = new ReplSetTest({
        name: jsTestName(),
        nodes: [{}, {rsConfig: {arbiterOnly: true}}],
    });
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    const primary = rst.getPrimary();
    assertVoteCount(primary, {
        votingMembersCount: 2,
        majorityVoteCount: 2,
        writableVotingMembersCount: 1,
        writeMajorityCount: 1,
        totalMembersCount: 2,
    });

    const config = rst.getReplSetConfigFromNode();
    jsTestLog("Original config: " + tojson(config));

    // This new node will be a secondary with {votes: 1, priority: 1}, which should not be able to
    // be added in reconfig if the new topology has a PSA architecture.
    rst.add({});
    const newConfig = rst.getReplSetConfig();
    config.members = newConfig.members;
    config.version += 1;
    jsTestLog(`New config with secondary added: ${tojson(config)}`);

    assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}),
                                 ErrorCodes.NewReplicaSetConfigurationIncompatible);

    // Verify that the vote counts have not changed, since the reconfig did not successfully
    // complete.
    assertVoteCount(primary, {
        votingMembersCount: 2,
        majorityVoteCount: 2,
        writableVotingMembersCount: 1,
        writeMajorityCount: 1,
        totalMembersCount: 2,
    });

    // Remove the node since it was not successfully added to the config, so we should not run
    // validation checks on it when we shut down the replica set.
    rst.remove(2);
    rst.stopSet();
}

{
    jsTestLog("Testing reconfig to remove {votes: 0} from secondary in PSA set fails");
    const rst = new ReplSetTest({
        nodes: [{}, {rsConfig: {votes: 0, priority: 0}}, {rsConfig: {arbiterOnly: true}}],
    });
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    const primary = rst.getPrimary();
    assertVoteCount(primary, {
        votingMembersCount: 2,
        majorityVoteCount: 2,
        writableVotingMembersCount: 1,
        writeMajorityCount: 1,
        totalMembersCount: 3,
    });

    const config = rst.getReplSetConfigFromNode();
    jsTestLog("Original config: " + tojson(config));

    // Modify the secondary to have {votes: 1, priority: 1}. This will also fail the reconfig.
    config.members[1].votes = 1;
    config.members[1].priority = 1;
    jsTestLog(
        `New config with secondary reconfigured to have {votes: 1, priority: 1}: 
              ${tojson(config)}`);

    assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}),
                                 ErrorCodes.NewReplicaSetConfigurationIncompatible);

    // Verify that the vote counts have not changed, since the reconfig did not successfully
    // complete.
    assertVoteCount(primary, {
        votingMembersCount: 2,
        majorityVoteCount: 2,
        writableVotingMembersCount: 1,
        writeMajorityCount: 1,
        totalMembersCount: 3,
    });

    rst.stopSet();
}

{
    jsTestLog(
        "Testing that the correct workflow for converting a replica set with only one writable voting node to a PSA architecture succeeds");
    const rst = new ReplSetTest({
        nodes: [{}, {rsConfig: {arbiterOnly: true}}],
    });
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    const primary = rst.getPrimary();
    assertVoteCount(primary, {
        votingMembersCount: 2,
        majorityVoteCount: 2,
        writableVotingMembersCount: 1,
        writeMajorityCount: 1,
        totalMembersCount: 2,
    });

    let config = rst.getReplSetConfigFromNode();
    jsTestLog("Original config: " + tojson(config));

    // First, add the secondary with {priority: 0}, so that it is not electable.
    rst.add({rsConfig: {votes: 1, priority: 0}});
    const newConfig = rst.getReplSetConfig();
    config.members = newConfig.members;
    config.version += 1;
    jsTestLog(`Reconfiguring set to add a secondary with {votes: 1: priority: 0. New config: ${
        tojson(config)}`);
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
    waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 2 /* memberIndex */);

    assertVoteCount(primary, {
        votingMembersCount: 3,
        majorityVoteCount: 2,
        writableVotingMembersCount: 2,
        writeMajorityCount: 2,
        totalMembersCount: 3
    });

    // Second, give the secondary a non-zero priority level.
    config = rst.getReplSetConfigFromNode();
    config.members[1].priority = 1;
    config.version += 1;
    jsTestLog(`Reconfiguring set to give the secondary a positive priority. New config: ${
        tojson(config)}`);
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

    assertVoteCount(primary, {
        votingMembersCount: 3,
        majorityVoteCount: 2,
        writableVotingMembersCount: 2,
        writeMajorityCount: 2,
        totalMembersCount: 3
    });

    rst.stopSet();
}
})();
