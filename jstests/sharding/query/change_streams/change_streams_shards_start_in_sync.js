// This test ensures that change streams on sharded collections start in sync with each other.
//
// As detailed in SERVER-31685, since shard cursors are not established simultaneously, it is
// possible that a sharded change stream could be established on shard 0, then write 'A' to shard 0
// could occur, followed by write 'B' to shard 1, and then the change stream could be established on
// shard 1, then some third write 'C' could occur.  This test ensures that in that case, both 'A'
// and 'B' will be seen in the changestream before 'C'.
// @tags: [
//   does_not_support_stepdowns,
//   requires_majority_read_concern,
//   uses_change_streams,
//   # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
//   embedded_router_incompatible,
// ]
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 2,
    rs: {
        nodes: 1,
        // Use a higher frequency for periodic noops to speed up the test.
        setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
    }
});

const buildInfo = assert.commandWorked(st.s0.adminCommand({"buildInfo": 1}));

// Check the build flags to determine whether we are running in a code-coverage variant. These
// variants are sufficiently slow as to interfere with the operation of this test, so we skip them.
if (buildInfo.buildEnvironment.ccflags.includes('-ftest-coverage')) {
    st.stop();
    jsTestLog("Skipping the test case run with code-coverage enabled");
    quit();
}

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
assert.commandWorked(
    mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.rs0.getURL()}));

// Shard the test collection on _id.
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

// Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

// Move the [0, MaxKey) chunk to st.shard1.shardName.
assert.commandWorked(mongosDB.adminCommand(
    {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

async function checkStream() {
    const {assertChangeStreamEventEq} = await import("jstests/libs/query/change_stream_util.js");

    const testDb = db.getSiblingDB(jsTestName());
    let coll = testDb[jsTestName()];
    let changeStream = coll.aggregate([{$changeStream: {}}], {comment: jsTestName()});

    assert.soon(() => changeStream.hasNext());
    assertChangeStreamEventEq(changeStream.next(), {
        documentKey: {_id: -1000},
        fullDocument: {_id: -1000},
        ns: {db: testDb.getName(), coll: coll.getName()},
        operationType: "insert",
    });

    assert.soon(() => changeStream.hasNext());
    assertChangeStreamEventEq(changeStream.next(), {
        documentKey: {_id: 1001},
        fullDocument: {_id: 1001},
        ns: {db: testDb.getName(), coll: coll.getName()},
        operationType: "insert",
    });

    assert.soon(() => changeStream.hasNext());
    assertChangeStreamEventEq(changeStream.next(), {
        documentKey: {_id: -1002},
        fullDocument: {_id: -1002},
        ns: {db: testDb.getName(), coll: coll.getName()},
        operationType: "insert",
    });
    changeStream.close();
}

// Helper function to list all idle cursors on the test namespace, with an optional extra filter.
function listIdleCursorsOnTestNs(rs, filter = {}) {
    return rs.getPrimary()
        .getDB('admin')
        .aggregate([
            {$currentOp: {idleCursors: true}},
            {
                $match: {
                    ns: mongosColl.getFullName(),
                    type: "idleCursor",
                }
            },
            {$match: filter}
        ])
        .toArray();
}

// Start the $changeStream and hang shard 1 when the mongos attempts to establish a $changeStream
// cursor on it.
const hangCursorEstablishingOnShard1 = configureFailPoint(
    st.shard1, "hangAfterAcquiringCollectionCatalog", {collection: mongosColl.getName()});
let waitForShell = startParallelShell(checkStream, st.s1.port);

// Helper function which waits for a $changeStream cursor to appear in currentOp on the given shard.
function waitForShardCursor(rs, n = 1) {
    assert.soon(() => listIdleCursorsOnTestNs(rs, {
                          "cursor.originatingCommand.aggregate": {$exists: true},
                          "cursor.originatingCommand.comment": jsTestName()
                      }).length === n,
                () => tojson(listIdleCursorsOnTestNs(rs)));
}

// Make sure the shard 0 $changeStream cursor is established before doing the first writes.
waitForShardCursor(st.rs0);

// Make sure that no $changeStream cursor is established on shard 1 yet.
waitForShardCursor(st.rs1, 0);

assert.commandWorked(mongosColl.insert({_id: -1000}, {writeConcern: {w: "majority"}}));

// This write to shard 1 occurs before the $changeStream cursor on shard 1 is open.
assert.commandWorked(mongosColl.insert({_id: 1001}, {writeConcern: {w: "majority"}}));

jsTestLog("Establishing $changeStream cursor on shard 1.");
hangCursorEstablishingOnShard1.off();
waitForShardCursor(st.rs1);

assert.commandWorked(mongosColl.insert({_id: -1002}, {writeConcern: {w: "majority"}}));
waitForShell();
st.stop();
