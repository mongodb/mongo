/**
 * Tests that a node can be successfully restarted when the bridge is enabled.
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
    "use strict";

    const name = "restart_node_with_bridge";
    const rst = new ReplSetTest({name: name, nodes: 1, useBridge: true});
    rst.startSet();
    rst.initiate();
    rst.awaitNodesAgreeOnPrimary();

    let primary = rst.getPrimary();
    assert.commandWorked(primary.getDB("test").getCollection(name).insert({_id: 1}));

    rst.restart(primary);
    rst.awaitNodesAgreeOnPrimary();
    primary = rst.getPrimary();
    assert.eq(primary.getDB("test").getCollection(name).count({_id: 1}), 1);

    rst.stopSet();
}());
