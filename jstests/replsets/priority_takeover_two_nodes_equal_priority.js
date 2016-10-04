// 2 nodes with non-default priority.
// 3-node replica set with priorities 3, 3 and 1 (default)
// Start replica set. Ensure that highest priority node becomes primary eventually.
// Step down the primary and confirm that the next highest priority node becomes primary.
load('jstests/replsets/rslib.js');

(function() {
    'use strict';

    var name = 'priority_takeover_two_nodes_equal_priority';
    var replSet = new ReplSetTest({
        name: name,
        nodes: [
            {rsConfig: {priority: 3}},
            {rsConfig: {priority: 3}},
            {},
        ]
    });
    replSet.startSet();
    replSet.initiate();

    var primary;
    var primaryIndex = -1;
    var defaultPriorityNodeIndex = 2;
    assert.soon(
        function() {
            primary = replSet.getPrimary();
            replSet.nodes.find(function(node, index, array) {
                if (primary.host == node.host) {
                    primaryIndex = index;
                    return true;
                }
                return false;
            });
            return primaryIndex !== defaultPriorityNodeIndex;
        },
        'neither of the priority 3 nodes was elected primary',
        60000,  // timeout
        1000    // interval
        );

    try {
        assert.commandWorked(primary.getDB('admin').runCommand({replSetStepDown: 90}));
    } catch (x) {
        // expected
    }
    var newPrimaryIndex = primaryIndex === 0 ? 1 : 0;

    // Refresh connections to nodes.
    replSet.status();

    assert.commandWorked(replSet.nodes[newPrimaryIndex].adminCommand({
        replSetTest: 1,
        waitForMemberState: ReplSetTest.State.PRIMARY,
        timeoutMillis: 60 * 1000,
    }),
                         'node ' + newPrimaryIndex + ' ' + replSet.nodes[newPrimaryIndex].host +
                             ' failed to become primary');

})();
