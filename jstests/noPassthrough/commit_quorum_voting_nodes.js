/**
 * Tests that index build commitQuorum can include non-voting data-bearing nodes.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
const replSet = new ReplSetTest({
    nodes: [
        {
            rsConfig: {
                tags: {es: 'dc1'},
            }
        },
        {
            rsConfig: {
                tags: {es: 'dc2'},
            }
        },
        {
            // Analytics node with zero votes should be included in a commitQuorum.
            rsConfig: {
                priority: 0,
                votes: 0,
                tags: {es: 'analytics'},
            },
        },
    ],
});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const config = primary.getDB('local').system.replset.findOne();

// Create a custom write concern for both nodes with the 'es' tag. We will expect this to be usable
// as an option to commitQuorum.
config.settings = {
    getLastErrorModes: {ESP: {"es": 3}}
};
config.version++;
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config}));

const db = replSet.getPrimary().getDB('test');
const coll = db['coll'];
assert.commandWorked(coll.insert({a: 1}));

// Shut the analytics node down so that it cannot contribute to the commitQuorum.
const analyticsNodeId = 2;
replSet.stop(analyticsNodeId, {forRestart: true});

// The default commitQuorum should not include the non-voting analytics node.
assert.commandWorked(coll.createIndex({a: 1}));
coll.dropIndexes();

// The explicit "votingMembers" should not include the non-voting analytics node.
assert.commandWorked(coll.createIndex({a: 1}, {}, "votingMembers"));
coll.dropIndexes();

// Restart the analytics node down so that it can contribute to the commitQuorum.
replSet.start(analyticsNodeId, {}, true /* restart */);

// This should include the non-voting analytics node.
const nNodes = replSet.nodeList().length;
assert.commandWorked(coll.createIndex({a: 1}, {}, nNodes));
coll.dropIndexes();

// This custom tag should include the analytics node.
assert.commandWorked(coll.createIndex({a: 1}, {}, "ESP"));
coll.dropIndexes();

// Not enough data-bearing nodes to satisfy commit quorum.
assert.commandFailedWithCode(coll.createIndex({a: 1}, {}, nNodes + 1),
                             ErrorCodes.UnsatisfiableCommitQuorum);
coll.dropIndexes();

replSet.stopSet();
})();
