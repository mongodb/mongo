/**
 * Test that when a primary is blocked in drain mode, catchup takeover can work even
 * if the primary has a lower config than the takeover node. The test starts a 3-node
 * replica set and then steps up node1 but blocks it in drain mode before it can bump
 * the config term. Next it steps up node2 and also blocks it in drain mode and later
 * unblocks node1 to let it finish config term bump so that it has higher config than
 * node2. Eventually after catchUpTakeoverDelayMillis has passed, node1 should be able
 * get the vote from node2 which has a lower config, and finish the catchup takeover.
 */

(function() {
'use strict';

load("jstests/libs/write_concern_util.js");
load('jstests/replsets/libs/election_metrics.js');

// Get the current config from the node and compare it with the provided config.
const getNodeConfigAndCompare = function(node, config, cmp) {
    const currentConfig = assert.commandWorked(node.adminCommand({replSetGetConfig: 1})).config;
    if (cmp === '=') {
        return currentConfig.term === config.term && currentConfig.version === config.version;
    } else if (cmp === '>') {
        return currentConfig.term > config.term ||
            (currentConfig.term === config.term && currentConfig.version > config.version);
    } else if (cmp === '<') {
        return currentConfig.term < config.term ||
            (currentConfig.term === config.term && currentConfig.version < config.version);
    } else {
        assert(false);
    }
};

// Wait for all nodes to acknowledge that the node at nodeIndex is in the specified state.
const waitForNodeState = function(nodes, nodeIndex, state, timeout) {
    assert.soon(() => {
        for (const node of nodes) {
            const status = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
            if (status.members[nodeIndex].state !== state) {
                return false;
            }
        }
        return true;
    }, `Failed to agree on node ${nodes[nodeIndex].host} in state ${state}`, timeout);
};

const replSet = new ReplSetTest({name: jsTestName(), nodes: 3});
const nodes = replSet.startSet();
let config = replSet.getReplSetConfig();
// Prevent nodes from syncing from other secondaries.
config.settings = {
    chainingAllowed: false,
};
replSet.initiateWithHighElectionTimeout(config);
replSet.awaitReplication();
assert.eq(replSet.getPrimary(), nodes[0]);

const statusBeforeTakeover =
    assert.commandWorked(nodes[1].adminCommand({serverStatus: 1, wiredTiger: 0}));

// Failpoint to hang node1 before the automatic reconfig on stepup bumps the config term.
const hangBeforeTermBumpFpNode1 = configureFailPoint(nodes[1], "hangBeforeReconfigOnDrainComplete");
const initialConfig = assert.commandWorked(nodes[0].adminCommand({replSetGetConfig: 1})).config;

// Stepup node1 and wait to hang before bumping the config term.
assert.commandWorked(nodes[1].adminCommand({replSetStepUp: 1}));
hangBeforeTermBumpFpNode1.wait();

// Wait for all nodes to acknowledge that node1 has become primary.
jsTestLog(`Waiting for all nodes to agree on ${nodes[1].host} being primary`);
replSet.awaitNodesAgreeOnPrimary(replSet.kDefaultTimeoutMS, nodes, nodes[1]);

// Check that the failpoint worked and the config has not changed.
assert(getNodeConfigAndCompare(nodes[1], initialConfig, '='));

// Stepup node2 and wait to hang before bumping the config term as well.
const hangBeforeTermBumpFpNode2 = configureFailPoint(nodes[2], "hangBeforeReconfigOnDrainComplete");
assert.commandWorked(nodes[2].adminCommand({replSetStepUp: 1}));
hangBeforeTermBumpFpNode2.wait();

// Wait for all nodes to acknowledge that node2 has become primary. Cannot use
// awaitNodesAgreeOnPrimary() or getPrimary() here which do not allow a node to
// see multiple primaries.
jsTestLog(`Waiting for all nodes to agree on ${nodes[2].host} being primary`);
waitForNodeState(nodes, 2, ReplSetTest.State.PRIMARY, 30 * 1000);

// Wait for node0 to change its sync source to node2. Later when the failpoint on node 1
// is lifted, it will do a no-op write and finish the stepup process, so its lastApplied
// opTime will be greater than the other two nodes. By waiting for sync source change we
// make sure node0 will not pull new entries from node1, making node1 the only eligible
// candidate to catchup takeover node2.
assert.soon(() => {
    const status = assert.commandWorked(nodes[0].adminCommand({replSetGetStatus: 1}));
    return status.syncSourceHost === nodes[2].host;
});

// Lift the failpoint on node1 to let it finish reconfig and bump the config term.
hangBeforeTermBumpFpNode1.off();

jsTestLog(`Waiting for ${nodes[1].host} to step down before doing catchup takeover.`);
waitForNodeState(nodes, 1, ReplSetTest.State.SECONDARY, 30 * 1000);

jsTestLog(
    `Waiting for ${nodes[1].host} to finish config term bump and propagate to ${nodes[0].host}`);
assert.soon(() => getNodeConfigAndCompare(nodes[0], initialConfig, '>'));
assert.soon(() => getNodeConfigAndCompare(nodes[1], initialConfig, '>'));
// Check that node2 is still in catchup mode, so it cannot install a new config.
assert(getNodeConfigAndCompare(nodes[2], initialConfig, '='));

// Wait for node1 to catchup takeover node2 after the default catchup takeover delay.
jsTestLog(`Waiting for ${nodes[1].host} to catchup takeover ${nodes[2].host}`);
waitForNodeState(nodes, 1, ReplSetTest.State.PRIMARY, 60 * 1000);

// Check again that node2 is still in catchup mode and has not installed a new config.
assert(getNodeConfigAndCompare(nodes[2], initialConfig, '='));

// Lift the failpoint on node2 and wait for all nodes to see node1 as the only primary.
hangBeforeTermBumpFpNode2.off();
replSet.awaitNodesAgreeOnPrimary(replSet.kDefaultTimeoutMS, nodes, nodes[1]);

// Check that election metrics has been updated with the new reason counter.
const statusAfterTakeover =
    assert.commandWorked(nodes[1].adminCommand({serverStatus: 1, wiredTiger: 0}));
verifyServerStatusElectionReasonCounterChange(statusBeforeTakeover.electionMetrics,
                                              statusAfterTakeover.electionMetrics,
                                              "catchUpTakeover",
                                              1);

replSet.stopSet();
})();
