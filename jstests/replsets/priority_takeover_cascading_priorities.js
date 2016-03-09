// Multiple voting members with varying levels of priority.
// 5-node replica set with priorities 5, 4, 3, 2 and 1 (default).
// Start replica set. Ensure that highest priority node becomes primary eventually.
// Shut down the primary and confirm that the next highest priority node becomes primary.
// Repeat until 3 nodes are left standing.
(function() {
    'use strict';
    load('jstests/replsets/rslib.js');

    var name = 'priority_takeover_cascading_priorities';
    var replSet = new ReplSetTest({
        name: name,
        nodes: [
            {rsConfig: {priority: 5}},
            {rsConfig: {priority: 4}},
            {rsConfig: {priority: 3}},
            {rsConfig: {priority: 2}},
            {rsConfig: {priority: 1}},
        ]
    });
    replSet.startSet();
    replSet.initiate();

    var waitForPrimary = function(i) {
        // Refresh connections to nodes.
        replSet.status();
        assert.commandWorked(
            replSet.nodes[i].adminCommand({
                replSetTest: 1,
                waitForMemberState: ReplSetTest.State.PRIMARY,
                timeoutMillis: 60 * 1000,
            }),
            'node ' + i + ' ' + replSet.nodes[i].host + ' failed to become primary');
    };

    waitForPrimary(0);
    // Wait until all nodes get the "no-op" of "new primary" after initial sync.
    waitUntilAllNodesCaughtUp(replSet.nodes);
    replSet.stop(0);

    waitForPrimary(1);
    replSet.stop(1);

    waitForPrimary(2);

    // Cannot stop any more nodes because we will not have a majority.
})();
