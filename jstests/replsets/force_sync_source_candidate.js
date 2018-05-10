/*
 * Tests that the 'forceSyncSourceCandidate' failpoint correctly forces a sync source.
 *
 * @tags: [requires_replication]
 */

(function() {
    "use strict";

    const failpointName = "forceSyncSourceCandidate";

    const rst = new ReplSetTest({
        nodes:
            [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        // Allow many initial sync attempts. Initial sync may fail if the sync source does not have
        // an oplog yet because it has not conducted its own initial sync yet.
        // We turn on the noop writer to encourage successful sync source selection.
        nodeOptions: {setParameter: {numInitialSyncAttempts: 100, writePeriodicNoops: true}}
    });
    const nodes = rst.startSet();

    function setFailPoint(node, syncSource) {
        const dataObj = {hostAndPort: syncSource.host};
        assert.commandWorked(node.adminCommand(
            {configureFailPoint: failpointName, mode: "alwaysOn", data: dataObj}));
    }

    setFailPoint(nodes[1], nodes[0]);
    setFailPoint(nodes[2], nodes[1]);
    setFailPoint(nodes[3], nodes[2]);

    rst.initiate();
    const primary = rst.getPrimary();

    rst.awaitSyncSource(nodes[1], nodes[0]);
    rst.awaitSyncSource(nodes[2], nodes[1]);
    rst.awaitSyncSource(nodes[3], nodes[2]);

    rst.stopSet();
})();
