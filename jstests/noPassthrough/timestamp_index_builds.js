/**
 * This test exercises a few important properties of timestamping index builds. First, it ensures
 * background index builds on primaries and secondaries timestamp both `init` and `commit`
 * operations on the catalog.
 *
 * Second, this test ensures the entire index build is ahead of the majority point. Thus when
 * restarting as a standalone, those indexes are not observed. When restarted with `--replSet`,
 * the indexes will be rebuilt. Currently performing a background index build at startup before
 * the logical clock is initialized will fail to timestamp index completion. The code currently
 * fixes this by foregrounding those index builds. We can observe the correctness of this behavior
 * by once again restarting the node as a standalone and not seeing any evidence of the second
 * index.
 *
 * This test does not guarantee that background index builds are foregrounded to correct
 * timestamping, merely that the catalog state is not corrupted due to the existence of background
 * index builds.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    name: "timestampingIndexBuilds",
    nodes: 2,
    nodeOptions: {setParameter: {logComponentVerbosity: tojsononeline({storage: {recovery: 2}})}}
});
const nodes = rst.startSet();
rst.initiate();

if (!rst.getPrimary().adminCommand("serverStatus").storageEngine.supportsSnapshotReadConcern) {
    // Only snapshotting storage engines require correct timestamping of index builds.
    rst.stopSet();
    return;
}

function getColl(conn) {
    return conn.getDB("timestampingIndexBuild")["coll"];
}

let coll = getColl(rst.getPrimary());

// Create a collection and wait for the stable timestamp to exceed its creation on both nodes.
assert.commandWorked(
    coll.insert({}, {writeConcern: {w: "majority", wtimeout: rst.kDefaultTimeoutMS}}));

// Wait for the stable timestamp to match the latest oplog entry on both nodes.
rst.awaitLastOpCommitted();

// Disable snapshotting on all members of the replica set so that further operations do not
// enter the majority snapshot.
nodes.forEach(node => assert.commandWorked(node.adminCommand(
                  {configureFailPoint: "disableSnapshotting", mode: "alwaysOn"})));

assert.commandWorked(coll.createIndexes([{foo: 1}], {background: true}));
rst.awaitReplication();

rst.stopSet(undefined, true);

// The `disableSnapshotting` failpoint is no longer in effect. Bring up and analyze each node
// separately. The client does not need to perform any writes from here on out.
for (let nodeIdx = 0; nodeIdx < 2; ++nodeIdx) {
    let node = nodes[nodeIdx];
    let nodeIdentity = tojsononeline({nodeIdx: nodeIdx, dbpath: node.dbpath, port: node.port});

    // Bringing up the node as a standalone should only find the `_id` index.
    {
        jsTestLog("Starting as a standalone. Ensure only the `_id` index exists. Node: " +
                  nodeIdentity);
        let conn = rst.start(nodeIdx, {noReplSet: true, noCleanData: true});
        assert.neq(null, conn, "failed to restart node");
        IndexBuildTest.assertIndexes(getColl(conn), 1, ['_id_']);
        rst.stop(nodeIdx);
    }

    // Bringing up the node with `--replSet` will run oplog recovery. The `foo` index will be
    // rebuilt, but not become "stable".
    {
        jsTestLog("Starting as a replica set. Both indexes should exist. Node: " + nodeIdentity);
        let conn = rst.start(nodeIdx, {startClean: false}, true);
        conn.setSlaveOk();
        IndexBuildTest.assertIndexes(getColl(conn), 2, ['_id_', 'foo_1']);
        rst.stop(nodeIdx);
    }

    // Restarting the node as a standalone once again only shows the `_id` index.
    {
        jsTestLog(
            "Starting as a standalone after replication startup recovery. Ensure only the `_id` index exists. Node: " +
            nodeIdentity);
        let conn = rst.start(nodeIdx, {noReplSet: true, noCleanData: true});
        assert.neq(null, conn, "failed to restart node");
        IndexBuildTest.assertIndexes(getColl(conn), 1, ['_id_']);
        rst.stop(nodeIdx);
    }
}

rst.stopSet();
}());
