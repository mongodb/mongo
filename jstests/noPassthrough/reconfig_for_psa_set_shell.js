/**
 * Tests the 'reconfigForPSASet()' shell function and makes sure that reconfig will succeed while
 * preserving majority reads.
 *
 * @tags: [requires_journaling]
 */

(function() {
'use strict';

load("jstests/replsets/rslib.js");

// Start up a PSA set with the secondary having 'votes: 0' and 'priority: 0'.
const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}, {rsConfig: {arbiterOnly: true}}],
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
assert.eq(primary, rst.nodes[0], "the primary should be the node at index 0");

// Verify that a reconfig that directly gives the secondary 'votes: 1' and 'priority: 1' will fail.
const config = rst.getReplSetConfigFromNode();
config.members[1].votes = 1;
config.members[1].priority = 1;

let reconfigScript = `assert.commandFailedWithCode(rs.reconfig(${
    tojson(config)}), ErrorCodes.NewReplicaSetConfigurationIncompatible)`;
let result = runMongoProgram('mongo', '--port', primary.port, '--eval', reconfigScript);
assert.eq(0, result, `reconfig did not fail with expected error code`);

const runReconfigForPSASet = (memberIndex, config, shouldSucceed, endPriority = 1) => {
    jsTestLog(`Testing with memberIndex ${memberIndex} and config ${tojson(config)}`);

    const reconfigScript =
        `assert.commandWorked(rs.reconfigForPSASet(${memberIndex}, ${tojson(config)}))`;
    const result = runMongoProgram('mongo', '--port', primary.port, '--eval', reconfigScript);
    if (shouldSucceed) {
        assert.eq(0, result, 'expected reconfigToPSASet to succeed, but it failed');

        // Wait for the new config to be committed.
        assert.soonNoExcept(() => isConfigCommitted(primary));

        const replSetGetConfig =
            assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
        assert.eq(1, replSetGetConfig.members[1].votes);
        assert.eq(endPriority, replSetGetConfig.members[1].priority);

        // Reset the config back to the original config.
        originalConfig.members[memberIndex].votes = 0;
        originalConfig.members[memberIndex].priority = 0;
        const reconfigToOriginalConfig =
            `assert.commandWorked(rs.reconfig(${tojson(originalConfig)}))`;
        assert.eq(
            0,
            runMongoProgram('mongo', '--port', primary.port, '--eval', reconfigToOriginalConfig));
        assert.soonNoExcept(() => isConfigCommitted(primary));
    } else {
        assert.neq(0, result, 'expected reconfigToPSASet to fail, but it succeeded');
    }
};

// Succeed with a reconfig to a standard PSA set, where the secondary has 'votes: 1' and 'priority:
// 1'.
jsTestLog("Testing reconfigForPSASet() succeeded: standard PSA set");
runReconfigForPSASet(1, config, true /* shouldSucceed */);

// Fail when 'memberIndex' does not refer to a node in the new config.
jsTestLog("Testing reconfigForPSASet() failed: memberIndex out of bounds");
runReconfigForPSASet(3, config, false /* shouldSucceed */);

// Fail when the node in the new config at 'memberIndex' is not a voter.
jsTestLog("Testing reconfigForPSASet() failed: node at memberIndex is not a voter");
config.members[1].votes = 0;
config.members[1].priority = 0;
runReconfigForPSASet(1, config, false /* shouldSucceed */);

// Test that reconfigToPSASet() will succeed when we are adding a new node at a specific
// 'memberIndex'.
jsTestLog("Testing reconfigForPSASet() succeeded: adding new node");

// First remove the node at index 1 to simulate a two-node replica set.
config = rst.getReplSetConfigFromNode();
const filteredMembers = config.members.filter(member => member._id !== 1);
config.members = filteredMembers;
config.version += 1;
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

const replSetGetConfig = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
assert.eq(1, replSetGetConfig.members[1].votes);
assert.eq(1, replSetGetConfig.members[1].priority);

rst.stopSet();
})();
