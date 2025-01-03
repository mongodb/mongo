/**
 * Contains helper functions for testing sync source selection and evaluation.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

/**
 * Asserts that the node is allowed to sync from 'syncSource'. The node is unable to sync from
 * 'syncSource' if it will create a cycle in the topology.
 */
export const assertNodeAllowedToSyncFromSource = (node, syncSource) => {
    const syncSourceStatus = assert.commandWorked(syncSource.adminCommand({replSetGetStatus: 1}));

    let currHost = syncSource.host;
    while (currHost) {
        // If the node is already one of the sync source's upstream nodes, a cycle will be
        // formed if the node syncs from syncSource. If so, we fail loudly.
        assert.neq(currHost, node.host);

        // Set 'currHost' to the next upstream node.
        const member = syncSourceStatus.members.find(member => (currHost === member.name));
        assert(member);
        currHost = member.syncSourceHost;
    }
};

/**
 * Forces 'node' to sync from 'syncSource' without using the 'replSetSyncFrom' command. The
 * 'forceSyncSourceCandidate' failpoint will be returned. The node will continue to sync from
 * 'syncSource' until the caller disables the failpoint.
 *
 * This function may result in a sync source cycle, even with the 'nodeAllowedToSyncFromSource'
 * check (for example, if the topology changes while the check is running). The caller of this
 * function should be defensive against this case.
 */
export const forceSyncSource = (rst, node, syncSource) => {
    const primary = rst.getPrimary();

    assert.neq(primary, node);
    assertNodeAllowedToSyncFromSource(node, syncSource);

    jsTestLog(`Forcing node ${node} to sync from ${syncSource}`);

    // Stop replication on the node, so that we can advance the optime on the sync source.
    const stopReplProducer = configureFailPoint(node, "stopReplProducer");
    const forceSyncSource =
        configureFailPoint(node, "forceSyncSourceCandidate", {"hostAndPort": syncSource.host});

    const primaryDB = primary.getDB("forceSyncSourceDB");
    const primaryColl = primaryDB["forceSyncSourceColl"];

    // The node will not replicate this write. This is necessary to ensure that the sync source
    // is ahead of us, so that we can accept it as our sync source.
    assert.commandWorked(primaryColl.insert({"forceSyncSourceWrite": "1"}));
    rst.awaitReplication(null, null, [syncSource]);

    stopReplProducer.wait();
    stopReplProducer.off();

    // Verify that the sync source is correct.
    forceSyncSource.wait();
    rst.awaitSyncSource(node, syncSource, 60 * 1000);

    return forceSyncSource;
};

/**
 * Asserts that the sync source of the given node will match syncSourceName soon. Additional
 * arguments are passed to the assert.soon.
 */
export const assertSyncSourceMatchesSoon = (node, syncSourceName, ...assertSoonArgs) => {
    return assert.soon(() => {
        const res = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
        return res.syncSourceHost === syncSourceName;
    }, ...assertSoonArgs);
};

/**
 * Pauses replication, inserts a document, unpause replication and calls
 * assertSyncSourceMatchesSoon.
 *
 * Warning: Works only if `expectedSyncSource` is the only voting node other than `secondary`.
 */
export function assertSyncSourceChangesTo(rst, secondary, expectedSyncSource) {
    // We need to wait for a heartbeat from the secondary to the sync source, then run sync
    // source selection, because:
    // 1) The sync source changes only after retrieving a batch and
    // 2) The sync source won't change if the secondary isn't behind the expected sync source, as
    //    determined by heartbeats.

    // Insert a document while 'secondary' is not replicating to force it to run
    // shouldChangeSyncSource.
    stopServerReplication(secondary);
    assert.commandWorked(
        rst.getPrimary().getDB("testSyncSourceChangesDb").getCollection("coll").insert({a: 1}, {
            writeConcern: {w: 1}
        }));
    const sourceId = rst.getNodeId(expectedSyncSource);
    // Waits for the secondary to see the expected sync source advance beyond it.
    assert.soon(function() {
        const status = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
        const appliedTimestamp = status.optimes.appliedOpTime.ts;
        const sourceMember = status.members.find((x) => x._id == sourceId);
        return timestampCmp(sourceMember.optime.ts, appliedTimestamp) > 0;
    });
    restartServerReplication(secondary);
    assertSyncSourceMatchesSoon(secondary, expectedSyncSource.host);
}

export const DataCenter = class {
    constructor(name, nodes) {
        this.name = name;
        this.nodes = nodes;
    }
};

/**
 * Sets a delay between every node in 'firstDataCenter' and every node in 'secondDataCenter'.
 */
export const delayMessagesBetweenDataCenters = (firstDataCenter, secondDataCenter, delayMillis) => {
    const firstDataCenterNodes = firstDataCenter.nodes;
    const secondDataCenterNodes = secondDataCenter.nodes;

    firstDataCenterNodes.forEach(node => {
        node.delayMessagesFrom(secondDataCenterNodes, delayMillis);
    });
    secondDataCenterNodes.forEach(node => {
        node.delayMessagesFrom(firstDataCenterNodes, delayMillis);
    });

    jsTestLog(`Delaying messages between ${firstDataCenter.name} and ${secondDataCenter.name} by ${
        delayMillis} milliseconds.`);
};
