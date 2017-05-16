// Priority (1 member with non-default priority).
// 3-node replica set - one arbiter and two electable nodes with different priorities.
// Wait for replica set to stabilize with higher priority node as primary.
// Step down high priority node. Wait for the lower priority electable node to become primary.
// Eventually high priority node will run a priority takeover election to become primary.
(function() {
    'use strict';
    load('jstests/replsets/rslib.js');

    var name = 'priority_takeover_one_node_higher_priority';
    var replSet = new ReplSetTest({
        name: name,
        nodes: [
            {rsConfig: {priority: 3}},
            {},
            {rsConfig: {arbiterOnly: true}},
        ]
    });
    replSet.startSet();
    replSet.initiate();

    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY);
    var primary = replSet.getPrimary();

    replSet.awaitSecondaryNodes();
    replSet.awaitReplication();

    // Primary should step down long enough for election to occur on secondary.
    var config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
    var stepDownException = assert.throws(function() {
        var result = primary.adminCommand({replSetStepDown: replSet.kDefaultTimeoutMS / 1000});
        print('replSetStepDown did not throw exception but returned: ' + tojson(result));
    });
    assert(isNetworkError(stepDownException),
           'replSetStepDown did not disconnect client; failed with ' + tojson(stepDownException));

    // Step down primary and wait for node 1 to be promoted to primary.
    replSet.waitForState(replSet.nodes[1], ReplSetTest.State.PRIMARY);

    // Unfreeze node 0 so it can seek election.
    assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));

    // Eventually node 0 will stand for election again because it has a higher priorty.
    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY);
})();
