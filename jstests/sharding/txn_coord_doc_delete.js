/**
 * Tests that the transaction coordinator deletes its state document in using write concern w:1
 * not majority.
 *
 * @tags: [uses_transactions]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    },
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.getCollection(ns).insert({x: 0}));

const lsid = {id: UUID()};
let txnNumber = 0;
const coordinatorPrimary = st.shard0;
const coordinatorRS = st.rs0;

function coordinatorDocExists(conn, lsid, txnNumber) {
    return (
        conn
            .getCollection("config.transaction_coordinators")
            .findOne({"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber}) !== null
    );
}

jsTest.log("Run a multi-shard transaction and commit with w: 'majority'");
const docs = [{x: -1}, {x: 1}];
assert.commandWorked(
    st.s.getDB(dbName).runCommand({
        insert: collName,
        documents: docs,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

const hangBeforeDeleteFp = configureFailPoint(coordinatorPrimary, "hangBeforeDeletingCoordinatorDoc");

const commitThread = new Thread(
    (host, lsidId, txnNumber) => {
        const conn = new Mongo(host);
        assert.commandWorked(
            conn.getDB("admin").runCommand({
                commitTransaction: 1,
                lsid: {id: UUID(lsidId)},
                txnNumber: NumberLong(txnNumber),
                autocommit: false,
                writeConcern: {w: "majority"},
            }),
        );
    },
    st.s.host,
    extractUUIDFromObject(lsid.id),
    txnNumber,
);
commitThread.start();

jsTest.log("Wait until the coordinator is about to delete its state doc");
hangBeforeDeleteFp.wait();

// Wait for all the steps the primary has taken so far, including steps involved
// in coordinating a cross-shard transaction, to be applied on the secondaries.
coordinatorRS.awaitReplication();

jsTest.log("Stopping replication on all secondaries of the coordinator replica set");
stopServerReplication(coordinatorRS.getSecondaries());

jsTest.log("Waiting for the commitTransaction command to return");
commitThread.join();

jsTest.log("Unpause the coordinator so it can perform the delete");
hangBeforeDeleteFp.off();

jsTest.log(
    "Wait for the state doc to get deleted on the primary. If the delete used " +
        'w: "majority", the wait would time out.',
);
// Verify the state doc eventually gets deleted on the primary.
assert.soon(() => !coordinatorDocExists(coordinatorPrimary, lsid, txnNumber));
// Verify that the state doc still exists on the secondaries.
coordinatorRS.getSecondaries().forEach((node) => {
    assert(coordinatorDocExists(node, lsid, txnNumber));
});

jsTest.log("Restart replication and verify that the delete is replicated to the secondaries");
restartServerReplication(coordinatorRS.getSecondaries());
coordinatorRS.awaitReplication();

coordinatorRS.nodes.forEach((node) => {
    assert(
        !coordinatorDocExists(node, lsid, txnNumber),
        "Coordinator doc should be deleted on all nodes: " + node.host,
    );
});

jsTest.log("Verify that the transaction committed");
const res = assert.commandWorked(st.s.getDB(dbName).runCommand({find: collName, filter: {$or: docs}, lsid: lsid}));
assert.eq(2, res.cursor.firstBatch.length);

st.stop();
