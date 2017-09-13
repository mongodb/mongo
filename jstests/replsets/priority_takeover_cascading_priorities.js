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

    replSet.waitForState(0, ReplSetTest.State.PRIMARY);
    // Wait until all nodes get the "no-op" of "new primary" after initial sync.
    waitUntilAllNodesCaughtUp(replSet.nodes);
    replSet.stop(0);

    replSet.waitForState(1, ReplSetTest.State.PRIMARY);
    replSet.stop(1);

    replSet.waitForState(2, ReplSetTest.State.PRIMARY);

    // Cannot stop any more nodes because we will not have a majority.
})();
