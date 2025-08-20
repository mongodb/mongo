/**
 * Tests that if a node is in catchup for too long (before exiting drain mode),
 * and another node takes over and becomes primary, that the former node will
 * safely exit drain mode and step down.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({
    nodes: [
        {},
        {},
        // Prevent node 2 from trying to run for election.
        {rsConfig: {priority: 0}},
    ],
    useBridge: true,
    settings: {chainingAllowed: true, catchupTimeoutMillis: -1},
});

const nodes = rst.startSet();
assert.eq(nodes.length, 3);
rst.initiate();
const dbs = nodes.map((node) => node.getDB(dbName));
const colls = dbs.map((db) => db.getCollection(collName));

function insertDocument(db) {
    return db.runCommand({
        insert: collName,
        documents: [{foo: "bar"}],
        writeConcern: {w: 1},
    });
}

// Make node 1 the primary.
rst.stepUp(nodes[1]);

// Ensure that all nodes are in the same term before enabling failpoints.
rst.awaitReplication();

// Set up a stopReplProducer failpoint on nodes 0 and 2.
// Node 0 needs to be behind node 1 in order for it to enter drain mode.
// Node 2 cannot be ahead of node 0 in order for node 0 to win its election.
const node0SrpFailpoint = configureFailPoint(nodes[0], "stopReplProducer");
const node2SrpFailpoint = configureFailPoint(nodes[2], "stopReplProducer");

// Insert a document on node 1 with w: 1.
const insertResponse = insertDocument(dbs[1]);
const insertTimestamp = insertResponse.operationTime;

// Set up a hangBeforeRSTLOnDrainComplete failpoint on node 0 to make it hang
// during drain mode.
const node0DrainModeFailpoint = configureFailPoint(
    nodes[0],
    "hangBeforeRSTLOnDrainComplete",
    {},
    "alwaysOn",
);

// Wait for node 0 to become aware of the insert via heartbeats.
assert.soon(() => {
    const replSetGetStatus = assert.commandWorked(
        nodes[0].adminCommand({replSetGetStatus: 1}),
    );
    return (bsonWoCompare(
                replSetGetStatus.$clusterTime.clusterTime,
                insertTimestamp,
                ) >= 0);
});

// Step up node 0 in a parallel shell.
// The shell will join after catchup completes, but before drain mode completes.
const shell = startParallelShell(
    () => assert.soonNoExcept(() => db.adminCommand({replSetStepUp: 1}).ok),
    nodes[0].port,
);
shell();

// Wait for node 0 to enter drain mode.
node0SrpFailpoint.off();
node0DrainModeFailpoint.wait();

// Assert that node 0 was able to successfully replicate the insert.
assert.eq("bar", colls[0].find({}).readConcern("local").toArray()[0].foo);

// Step up node 1.
rst.stepUp(nodes[1], {awaitReplicationBeforeStepUp: false});
rst.awaitNodesAgreeOnPrimary(rst.timeoutMS, nodes, nodes[1]);

// Disable the failpoints on node 0.
node0DrainModeFailpoint.off();

// Ensure that node 0 has stepped down.
const res = nodes[0].adminCommand({hello: 1});
assert.eq(res.isWritablePrimary, false);
assert.eq(res.secondary, true);

node2SrpFailpoint.off();

rst.stopSet();
