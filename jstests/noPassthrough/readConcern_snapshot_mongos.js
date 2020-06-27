// Test parsing of readConcern level 'snapshot' and the presence of the 'atClusterTime' field in
// snapshot cursor responses on mongos.
// @tags: [
//    requires_persistence,
//    requires_replication,
//    requires_sharding,
//    uses_atclustertime,
//    uses_transactions,
// ]
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");

// Runs the command as the first in a multi statement txn that is aborted right after, expecting
// success.
function expectSuccessInTxnThenAbort(session, sessionConn, cmdObj) {
    session.startTransaction();
    let res = assert.commandWorked(sessionConn.runCommand(cmdObj));
    // Transaction reads should not have 'atClusterTime' field in responses.
    if (res.hasOwnProperty("cursor")) {
        assert(!res.cursor.hasOwnProperty("atClusterTime"), tojson(res));
    } else {
        assert(!res.hasOwnProperty("atClusterTime"), tojson(res));
    }
    assert.commandWorked(session.abortTransaction_forTesting());
}

const dbName = "test";
const collName = "coll";

let st = new ShardingTest({shards: 1, rs: {nodes: 2}, config: 2, mongos: 1});
let testDB = st.getDB(dbName);

// Insert data to create the collection.
assert.commandWorked(testDB[collName].insert({x: 1}));

flushRoutersAndRefreshShardMetadata(st, {ns: dbName + "." + collName, dbNames: [dbName]});

// Test snapshot in a transaction.
let session = testDB.getMongo().startSession({causalConsistency: false});
let sessionDb = session.getDatabase(dbName);

// readConcern 'snapshot' is supported by insert on mongos in a transaction.
expectSuccessInTxnThenAbort(session, sessionDb, {
    insert: collName,
    documents: [{_id: "single-insert"}],
    readConcern: {level: "snapshot"},
});

// readConcern 'snapshot' is supported by update on mongos in a transaction.
expectSuccessInTxnThenAbort(session, sessionDb, {
    update: collName,
    updates: [{q: {_id: 0}, u: {$inc: {a: 1}}}],
    readConcern: {level: "snapshot"},
});

// readConcern 'snapshot' is supported by delete on mongos in a transaction.
expectSuccessInTxnThenAbort(session, sessionDb, {
    delete: collName,
    deletes: [{q: {}, limit: 1}],
    readConcern: {level: "snapshot"},
});

// readConcern 'snapshot' is supported by findAndModify on mongos in a transaction.
expectSuccessInTxnThenAbort(session, sessionDb, {
    findAndModify: collName,
    query: {},
    update: {$set: {a: 1}},
    readConcern: {level: "snapshot"},
});

// readConcern 'snapshot' is supported by aggregate on mongos in a transaction.
expectSuccessInTxnThenAbort(session, sessionDb, {
    aggregate: collName,
    pipeline: [],
    cursor: {},
    readConcern: {level: "snapshot"},
});

// readConcern 'snapshot' is supported by find and getMore on mongos in a transaction.
session.startTransaction();
let res = assert.commandWorked(
    sessionDb.runCommand({find: collName, batchSize: 0, readConcern: {level: "snapshot"}}));
assert(!res.cursor.hasOwnProperty("atClusterTime"));
res = assert.commandWorked(sessionDb.runCommand({getMore: res.cursor.id, collection: collName}));
assert(!res.cursor.hasOwnProperty("atClusterTime"));
assert.commandWorked(session.abortTransaction_forTesting());

// readConcern 'snapshot' is supported by distinct on mongos in a transaction.
expectSuccessInTxnThenAbort(session, sessionDb, {
    distinct: collName,
    key: "x",
    readConcern: {level: "snapshot"},
});

let pingRes = assert.commandWorked(st.s0.adminCommand({ping: 1}));
assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));
const clusterTime = pingRes.$clusterTime.clusterTime;

// readConcern 'snapshot' is allowed with 'afterClusterTime'.
expectSuccessInTxnThenAbort(session, sessionDb, {
    find: collName,
    readConcern: {level: "snapshot", afterClusterTime: clusterTime},
});

expectSuccessInTxnThenAbort(session, sessionDb, {
    aggregate: collName,
    pipeline: [],
    cursor: {},
    readConcern: {level: "snapshot", afterClusterTime: clusterTime},
});

// Test snapshot outside of transactions on mongos.
const snapshotReadConcern = {
    level: "snapshot"
};
// readConcern 'snapshot' is supported by find outside of transactions on mongos.
res = assert.commandWorked(
    testDB.runCommand({find: collName, batchSize: 0, readConcern: snapshotReadConcern}));
assert(res.cursor.hasOwnProperty("atClusterTime"), tojson(res));

// readConcern 'snapshot' is supported by getMore outside of transactions on mongos.
res = assert.commandWorked(testDB.runCommand({getMore: res.cursor.id, collection: collName}));
assert(res.cursor.hasOwnProperty("atClusterTime"), tojson(res));

// readConcern 'snapshot' is supported by aggregate outside of transactions on mongos.
res = assert.commandWorked(testDB.runCommand(
    {aggregate: collName, pipeline: [], cursor: {}, readConcern: snapshotReadConcern}));
assert(res.cursor.hasOwnProperty("atClusterTime"), tojson(res));

// readConcern 'snapshot' is supported by distinct outside of transactions on mongos.
res = assert.commandWorked(
    testDB.runCommand({distinct: collName, key: "x", readConcern: snapshotReadConcern}));
assert(res.hasOwnProperty("atClusterTime"), tojson(res));

// readConcern 'snapshot' is not supported by count on mongos.
assert.commandFailedWithCode(testDB.runCommand({count: collName, readConcern: snapshotReadConcern}),
                             ErrorCodes.InvalidOptions);

// readConcern 'snapshot' is not supported by findAndModify outside of transactions on mongos.
assert.commandFailedWithCode(testDB.runCommand({
    findAndModify: collName,
    query: {},
    update: {$set: {a: 1}},
    readConcern: snapshotReadConcern,
}),
                             ErrorCodes.InvalidOptions);

st.stop();
}());
