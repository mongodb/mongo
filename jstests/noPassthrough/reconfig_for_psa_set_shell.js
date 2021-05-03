/**
 * Tests the 'reconfigForPSASet()' shell function and makes sure that reconfig will succeed while
 * preserving majority reads.
 *
 * @tags: [requires_journaling]
 */

(function() {
'use strict';

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

// Verify that calling 'reconfigForPSASet()' will succeed.
reconfigScript = `assert.commandWorked(rs.reconfigForPSASet(1, ${tojson(config)}))`;
result = runMongoProgram('mongo', '--port', primary.port, '--eval', reconfigScript);
assert.eq(0, result, `reconfig did not succeed as expected`);

const replSetGetConfig = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
assert.eq(1, replSetGetConfig.members[1].votes);
assert.eq(1, replSetGetConfig.members[1].priority);

rst.stopSet();
})();
