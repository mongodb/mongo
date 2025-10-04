// Verifies basic sharded transaction behavior with the supported read concern levels.
//
// @tags: [
//   requires_sharding,
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});

// Set up a sharded collection with 2 chunks, one on each shard.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: -1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(st.s.getDB(dbName)[collName].insert({_id: 1}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName}));
st.refreshCatalogCacheForNs(st.s, ns);

// Refresh second shard to avoid stale shard version error on the second transaction statement.
assert.commandWorked(st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));

function runTest(st, readConcern, sessionOptions) {
    jsTestLog("Testing readConcern: " + tojson(readConcern) + ", sessionOptions: " + tojson(sessionOptions));

    const session = st.s.startSession(sessionOptions);
    const sessionDB = session.getDatabase(dbName);

    if (readConcern) {
        session.startTransaction({readConcern: readConcern});
    } else {
        session.startTransaction();
    }

    // Target only the first shard.
    assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: -1}}));

    // On a separate, causally consistent session, read from the first shard then write to the
    // second one. This write is guaranteed to commit at a later cluster time than that of the
    // snapshot established by the transaction on the first shard.
    const otherSessionDB = st.s.startSession().getDatabase(dbName);
    assert.commandWorked(otherSessionDB.runCommand({find: collName}));
    assert.commandWorked(otherSessionDB.runCommand({insert: collName, documents: [{_id: 5}]}));
    // Depending on the transaction's read concern, the new document will or will not be visible
    // to the next statement.
    const numExpectedDocs = readConcern && readConcern.level === "snapshot" ? 0 : 1;
    assert.eq(
        numExpectedDocs,
        sessionDB[collName].find({_id: 5}).itcount(),
        "sharded transaction with read concern " +
            tojson(readConcern) +
            " did not see expected number of documents, sessionOptions: " +
            tojson(sessionOptions),
    );

    assert.commandWorked(session.commitTransaction_forTesting());

    // Clean up for the next iteration.
    assert.commandWorked(sessionDB[collName].remove({_id: 5}));
}

// Specifying no read concern level is allowed and should not compute a global snapshot.
runTest(st, undefined, {causalConsistency: false});
runTest(st, undefined, {causalConsistency: true});

const kAllowedReadConcernLevels = ["local", "majority", "snapshot"];
for (let readConcernLevel of kAllowedReadConcernLevels) {
    runTest(st, {level: readConcernLevel}, {causalConsistency: false});
    runTest(st, {level: readConcernLevel}, {causalConsistency: true});
}

st.stop();
