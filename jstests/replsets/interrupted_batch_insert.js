// Tests the scenario described in SERVER-27534.
// 1. Send a single insert command with a large number of documents and the {ordered: true} option.
// 2. Force the thread processing the insert command to hang in between insert batches. (Inserts are
//    typically split into batches of 64, and the server yields locks between batches.)
// 3. Disconnect the original primary from the network, forcing another node to step up.
// 4. Insert a single document on the new primary.
// 5. Return the original primary to the network and force it to step up by disconnecting the
//    primary that replaced it. The original primary has to roll back any batches from step 1
//    that were inserted locally but did not get majority committed before the insert in step 4.
// 6. Unpause the thread performing the insert from step 1. If it continues to insert batches even
//    though there was a rollback, those inserts will violate the {ordered: true} option.

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

let name = "interrupted_batch_insert";
let replTest = new ReplSetTest({name: name, nodes: 3, useBridge: true});
let nodes = replTest.nodeList();

let conns = replTest.startSet();
replTest.initiate(
    {
        _id: name,
        members: [
            {_id: 0, host: nodes[0]},
            {_id: 1, host: nodes[1]},
            {_id: 2, host: nodes[2], priority: 0},
        ],
    },
    null,
    {initiateWithDefaultElectionTimeout: true},
);

// The test starts with node 0 as the primary.
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
let primary = replTest.nodes[0];
let collName = primary.getDB("db")[name].getFullName();
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
replTest.awaitReplication();

const batchSize = 500;
const setParameterResult = primary.getDB("admin").runCommand({setParameter: 1, internalInsertMaxBatchSize: batchSize});
assert.commandWorked(setParameterResult);

// Prevent any writes to node 0 (the primary) from replicating to nodes 1 and 2.
stopServerReplication(conns[1]);
stopServerReplication(conns[2]);

// Allow the primary to insert the first 5 batches of documents. After that, the fail point
// activates, and the client thread hangs until the fail point gets turned off.
let failPoint = configureFailPoint(
    primary.getDB("db"),
    "hangDuringBatchInsert",
    {shouldCheckForInterrupt: true},
    {skip: 5},
);

// In a background thread, issue an insert command to the primary that will insert 10 batches of
// documents.
let worker = new Thread(
    (host, collName, numToInsert) => {
        // Insert elements [{idx: 0}, {idx: 1}, ..., {idx: numToInsert - 1}].
        const docsToInsert = Array.from({length: numToInsert}, (_, i) => {
            return {idx: i};
        });
        let coll = new Mongo(host).getCollection(collName);
        assert.commandFailedWithCode(
            coll.insert(docsToInsert, {writeConcern: {w: "majority", wtimeout: 5000}, ordered: true}),
            ErrorCodes.InterruptedDueToReplStateChange,
        );
    },
    primary.host,
    collName,
    10 * batchSize,
);
worker.start();

// Wait long enough to guarantee that all 5 batches of inserts have executed and the primary is
// hung on the "hangDuringBatchInsert" fail point.
failPoint.wait();

// Make sure the insert command is, in fact, running in the background.
assert.eq(primary.getDB("db").currentOp({"command.insert": name, active: true}).inprog.length, 1);

// Completely isolate the current primary (node 0), forcing it to step down.
conns[0].disconnect(conns[1]);
conns[0].disconnect(conns[2]);

// Wait for node 1, the only other eligible node, to become the new primary.
replTest.waitForState(replTest.nodes[1], ReplSetTest.State.PRIMARY);
assert.eq(replTest.nodes[1], replTest.getPrimary());

restartServerReplication(conns[2]);

// Issue a write to the new primary.
let collOnNewPrimary = replTest.nodes[1].getCollection(collName);
assert.commandWorked(collOnNewPrimary.insert({singleDoc: 1}, {writeConcern: {w: "majority"}}));

// Isolate node 1, forcing it to step down as primary, and reconnect node 0, allowing it to step
// up again.
conns[1].disconnect(conns[2]);
conns[0].reconnect(conns[2]);

// Wait for node 0 to become primary again.
replTest.waitForState(primary, ReplSetTest.State.PRIMARY);
assert.eq(replTest.nodes[0], replTest.getPrimary());

// Allow the batch insert to continue.
failPoint.off();

// Wait until the insert command is done.
assert.soon(() => primary.getDB("db").currentOp({"command.insert": name, active: true}).inprog.length === 0);

worker.join();

let docs = primary
    .getDB("db")
    [name].find({idx: {$exists: 1}})
    .sort({idx: 1})
    .toArray();

// Any discontinuity in the "idx" values is an error. If an "idx" document failed to insert, all
// the of "idx" documents after it should also have failed to insert, because the insert
// specified {ordered: 1}. Note, if none of the inserts were successful, that's fine.
docs.forEach((element, index) => {
    assert.eq(element.idx, index);
});

// Reconnect the remaining disconnected nodes, so we can exit.
conns[0].reconnect(conns[1]);
conns[1].reconnect(conns[2]);
restartServerReplication(conns[1]);

replTest.stopSet(15);
