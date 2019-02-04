// 3-node replica set - one arbiter and two electable nodes with different priorities.
// Wait for replica set to stabilize with the higher priority node as primary.
// Step down the high priority node. Wait for the lower priority electable node to become primary.
// Eventually the high priority node will run a priority takeover election to become primary. During
// this election that node should make sure that it does not error in _requestRemotePrimaryStepDown.
(function() {
    'use strict';
    load('jstests/replsets/rslib.js');

    var name = 'request_primary_stepdown';
    var replSet = new ReplSetTest(
        {name: name, nodes: [{rsConfig: {priority: 3}}, {}, {rsConfig: {arbiterOnly: true}}]});
    replSet.startSet();
    replSet.initiate();

    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY);
    replSet.awaitSecondaryNodes();
    replSet.awaitReplication();
    var primary = replSet.getPrimary();

    assert.commandWorked(
        replSet.nodes[0].adminCommand({setParameter: 1, logComponentVerbosity: {executor: 4}}));
    clearRawMongoProgramOutput();

    // Primary should step down long enough for election to occur on secondary.
    assert.commandWorked(
        primary.adminCommand({replSetStepDown: 70, secondaryCatchUpPeriodSecs: 60}));

    // Wait for node 1 to be promoted to primary after node 0 stepped down.
    replSet.waitForState(replSet.nodes[1], ReplSetTest.State.PRIMARY, 60 * 1000);

    // Eventually node 0 will stand for election again because it has a higher priorty.
    replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY, 100 * 1000);
    var logContents = rawMongoProgramOutput();
    assert.eq(logContents.indexOf("stepdown period must be longer than secondaryCatchUpPeriodSecs"),
              -1,
              "_requestRemotePrimaryStepDown sent an invalid replSetStepDown command");
    replSet.stopSet();
})();
