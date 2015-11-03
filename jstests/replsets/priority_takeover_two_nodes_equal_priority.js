// 2 nodes with non-default priority.
// 3-node replica set with priorities 3, 3 and 1 (default)
// Start replica set. Ensure that highest priority node becomes primary eventually.
// Shut down the primary and confirm that the next highest priority node becomes primary.
// Repeat until 2 nodes are left standing.
(function () {
    'use strict';
    load('jstests/replsets/rslib.js');

    var name = 'priority_takeover_two_nodes_equal_priority';
    var replSet = new ReplSetTest({name: name, nodes: [
        {rsConfig: {priority: 3}},
        {rsConfig: {priority: 3}},
        {},
    ]});
    replSet.startSet();
    replSet.initiate();

    var primary = replSet.getPrimary();
    var primaryIndex = -1;
    replSet.nodes.find(function(node, index, array) {
        if (primary.host == node.host) {
            primaryIndex = index;
            return true;
        }
        return false;
    });
    assert.neq(-1, primaryIndex,
               'expected one of the nodes with priority 3 to become primary');

    replSet.stop(primaryIndex);
    var newPrimaryIndex = primaryIndex === 0 ? 1 : 0;

    // Refresh connections to nodes.
    replSet.status();
    assert.commandWorked(
        replSet.nodes[newPrimaryIndex].adminCommand({
            replSetTest: 1,
            waitForMemberState: replSet.PRIMARY,
            timeoutMillis: 60 * 1000,
        }),
        'node ' + newPrimaryIndex + ' ' + replSet.nodes[newPrimaryIndex].host +
        ' failed to become primary'
    );
})();
