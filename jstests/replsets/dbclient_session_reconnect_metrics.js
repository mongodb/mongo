/**
 * Test that reconnect metrics for DBClientSession exist and are appropriately incremented.
 *
 * This test cannot be run on older binaries because the metrics do not exist yet.
 * * @tags: [multiversion_incompatible]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

function getMetrics(conn) {
    const status = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    return Object.fromEntries(
        ["dbClientSessionReconnectAttempts", "dbClientSessionWithoutAutoReconnectFailures"].map((name) => [
            name,
            status.metrics.network[name],
        ]),
    );
}

const rst = new ReplSetTest({name: "testSet", useBridge: true, nodes: 3, settings: {chainingAllowed: false}});
const nodes = rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
rst.awaitSecondaryNodes();
const secondary = rst.getSecondaries()[0];

const before_disconnect = getMetrics(secondary);
// dbClientSessionWithoutAutoReconnectFailures should never be incremented because the connection
// created by the oplog fetcher is always created with autoReconnect = true.
assert.eq(before_disconnect.dbClientSessionWithoutAutoReconnectFailures, 0);

secondary.disconnect(primary);
assert.soon(() => {
    const after_disconnect = getMetrics(secondary);
    return after_disconnect.dbClientSessionReconnectAttempts > before_disconnect.dbClientSessionReconnectAttempts;
});

rst.stopSet();
