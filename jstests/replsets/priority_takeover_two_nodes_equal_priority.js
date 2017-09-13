/**
 * Test to ensure that nodes with the highest priorities eventually become PRIMARY.
 *
 * 1. Initiate a 3 node replica set with node priorities of 3, 3 and 1 (default)
 * 2. Make sure that one of the highest priority nodes becomes PRIMARY.
 * 3. Step down the PRIMARY and confirm that the other high priority node becomes PRIMARY.
 */
load('jstests/replsets/rslib.js');

(function() {
    'use strict';

    var name = 'priority_takeover_two_nodes_equal_priority';
    var replTest = new ReplSetTest(
        {name: name, nodes: [{rsConfig: {priority: 3}}, {rsConfig: {priority: 3}}, {}]});
    replTest.startSet();
    replTest.initiate();

    jsTestLog("Waiting for one of the high priority nodes to become PRIMARY.");
    var primary;
    var primaryIndex = -1;
    var defaultPriorityNodeIndex = 2;
    assert.soon(
        function() {
            primary = replTest.getPrimary();
            replTest.nodes.find(function(node, index, array) {
                if (primary.host == node.host) {
                    primaryIndex = index;
                    return true;
                }
                return false;
            });
            return primaryIndex !== defaultPriorityNodeIndex;
        },
        'Neither of the high priority nodes was elected primary.',
        replTest.kDefaultTimeoutMS,  // timeout
        1000                         // interval
        );

    jsTestLog("Stepping down the current primary.");
    assert.throws(function() {
        assert.commandWorked(
            primary.adminCommand({replSetStepDown: 10 * 60, secondaryCatchUpPeriodSecs: 10 * 60}));
    });

    // Make sure the primary has stepped down.
    assert.neq(primary, replTest.getPrimary());

    // We expect the other high priority node to eventually become primary.
    var expectedNewPrimaryIndex = (primaryIndex === 0) ? 1 : 0;

    jsTestLog("Waiting for the other high priority node to become PRIMARY.");
    var expectedNewPrimary = replTest.nodes[expectedNewPrimaryIndex];
    replTest.waitForState(expectedNewPrimary, ReplSetTest.State.PRIMARY);

})();
