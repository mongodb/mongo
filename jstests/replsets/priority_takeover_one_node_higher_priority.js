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

    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY, 60 * 1000);
    var primary = replSet.getPrimary();

    replSet.awaitSecondaryNodes();
    replSet.awaitReplication();

    // Primary should step down long enough for election to occur on secondary.
    var config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
    var electionTimeoutMillis = config.settings.electionTimeoutMillis;
    var stepDownGuardMillis = electionTimeoutMillis * 2;
    var stepDownException = assert.throws(function() {
        var result = primary.adminCommand({replSetStepDown: stepDownGuardMillis / 1000});
        print('replSetStepDown did not throw exception but returned: ' + tojson(result));
    });
    assert.neq(-1,
               tojson(stepDownException).indexOf('error doing query'),
               'replSetStepDown did not disconnect client');

    // Step down primary and wait for node 1 to be promoted to primary.
    replSet.waitForState(replSet.nodes[1], ReplSetTest.State.PRIMARY, 60 * 1000);

    // Eventually node 0 will stand for election again because it has a higher priorty.
    replSet.waitForState(
        replSet.nodes[0], ReplSetTest.State.PRIMARY, stepDownGuardMillis + 60 * 1000);
})();
