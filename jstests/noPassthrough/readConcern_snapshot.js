// Test parsing of readConcern level 'snapshot'.
// @tags: [requires_majority_read_concern, requires_replication, uses_transactions]
(function() {
"use strict";

const dbName = "test";
const collName = "coll";

//
// Configurations.
//

// Transactions should fail on storage engines that do not support them.
let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
let session = rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
let sessionDb = session.getDatabase(dbName);
if (!sessionDb.serverStatus().storageEngine.supportsSnapshotReadConcern) {
    // Transactions with readConcern snapshot fail.
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}),
                                 ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 [ErrorCodes.NoSuchTransaction, ErrorCodes.IllegalOperation]);

    // Transactions without readConcern snapshot fail.
    session.startTransaction();
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}),
                                 ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 [ErrorCodes.NoSuchTransaction, ErrorCodes.IllegalOperation]);

    rst.stopSet();
    return;
}
session.endSession();
rst.stopSet();

// readConcern 'snapshot' is not allowed on a standalone.
const conn = MongoRunner.runMongod();
session = conn.startSession({causalConsistency: false});
sessionDb = session.getDatabase(dbName);
assert.neq(null, conn, "mongod was unable to start up");
session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.IllegalOperation);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.IllegalOperation);
session.endSession();
MongoRunner.stopMongod(conn);

// readConcern 'snapshot' is allowed on a replica set primary.
rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
assert.commandWorked(
    rst.getPrimary().getDB(dbName).runCommand({create: collName, writeConcern: {w: "majority"}}));
session = rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
sessionDb = session.getDatabase(dbName);
session.startTransaction({writeConcern: {w: "majority"}, readConcern: {level: "snapshot"}});
assert.commandWorked(sessionDb.coll.insert({}));
assert.commandWorked(sessionDb.runCommand({find: collName}));
assert.commandWorked(session.commitTransaction_forTesting());

// readConcern 'snapshot' is allowed with 'afterClusterTime'.
session.startTransaction();
let pingRes = assert.commandWorked(rst.getPrimary().adminCommand({ping: 1}));
assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));
assert.commandWorked(sessionDb.runCommand({
    find: collName,
    readConcern: {level: "snapshot", afterClusterTime: pingRes.$clusterTime.clusterTime}
}));
assert.commandWorked(session.commitTransaction_forTesting());

// readConcern 'snapshot' is not allowed with 'afterOpTime'.
session.startTransaction(
    {readConcern: {level: "snapshot", afterOpTime: {ts: Timestamp(1, 2), t: 1}}});
assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
session.endSession();

pingRes = assert.commandWorked(rst.getSecondary().adminCommand({ping: 1}));
assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));

session.startTransaction(
    {readConcern: {level: "snapshot", afterClusterTime: pingRes.$clusterTime.clusterTime}});
assert.commandWorked(sessionDb.runCommand({find: collName}));
assert.commandWorked(session.commitTransaction_forTesting());

session.endSession();
rst.stopSet();

//
// Commands.
//

rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
let testDB = rst.getPrimary().getDB(dbName);
let coll = testDB.coll;
assert.commandWorked(coll.createIndex({geo: "2d"}));
assert.commandWorked(testDB.runCommand({
    createIndexes: collName,
    indexes: [{key: {haystack: "geoHaystack", a: 1}, name: "haystack_geo", bucketSize: 1}],
    writeConcern: {w: "majority"}
}));

// Test snapshot in a transaction.
session = testDB.getMongo().startSession({causalConsistency: false});
sessionDb = session.getDatabase(dbName);

// readConcern 'snapshot' is supported by find in a transaction.
session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
assert.commandWorked(sessionDb.runCommand({find: collName}));

// readConcern 'snapshot' is supported by aggregate in a transaction.
assert.commandWorked(sessionDb.runCommand({aggregate: collName, pipeline: [], cursor: {}}));

// readConcern 'snapshot' is supported by distinct in a transaction.
assert.commandWorked(sessionDb.runCommand({distinct: collName, key: "x"}));

// readConcern 'snapshot' is supported by geoSearch in a transaction.
assert.commandWorked(
    sessionDb.runCommand({geoSearch: collName, near: [0, 0], maxDistance: 1, search: {a: 1}}));

// readConcern 'snapshot' is not supported by non-CRUD commands in a transaction.
assert.commandFailedWithCode(sessionDb.runCommand({dropIndexes: collName, index: "a_1"}),
                             ErrorCodes.OperationNotSupportedInTransaction);
assert.commandWorked(session.abortTransaction_forTesting());
session.endSession();

// Test snapshot outside of transactions.
const snapshotReadConcern = {
    level: "snapshot"
};
// readConcern 'snapshot' is supported by find outside of transactions.
assert.commandWorked(testDB.runCommand({find: collName, readConcern: snapshotReadConcern}));

// readConcern 'snapshot' is supported by aggregate outside of transactions.
assert.commandWorked(testDB.runCommand(
    {aggregate: collName, pipeline: [], cursor: {}, readConcern: snapshotReadConcern}));

// readConcern 'snapshot' is supported by distinct outside of transactions.
assert.commandWorked(
    testDB.runCommand({distinct: collName, key: "x", readConcern: snapshotReadConcern}));

// readConcern 'snapshot' is not supported by geoSearch outside of transactions.
assert.commandFailedWithCode(testDB.runCommand({
    geoSearch: collName,
    near: [0, 0],
    maxDistance: 1,
    search: {a: 1},
    readConcern: snapshotReadConcern
}),
                             ErrorCodes.InvalidOptions);

// readConcern 'snapshot' is not supported by count.
assert.commandFailedWithCode(testDB.runCommand({count: collName, readConcern: snapshotReadConcern}),
                             ErrorCodes.InvalidOptions);

// readConcern 'snapshot' is not supported by findAndModify.
assert.commandFailedWithCode(testDB.runCommand({
    findAndModify: collName,
    query: {},
    update: {$set: {a: 1}},
    readConcern: snapshotReadConcern,
}),
                             ErrorCodes.InvalidOptions);

// readConcern 'snapshot' is not supported by non-CRUD commands.
assert.commandFailedWithCode(
    testDB.adminCommand({listDatabases: 1, readConcern: snapshotReadConcern}),
    ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(
    testDB.runCommand({listCollections: 1, readConcern: snapshotReadConcern}),
    ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(
    testDB.runCommand({listIndexes: collName, readConcern: snapshotReadConcern}),
    ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(
    testDB.runCommand({dropIndexes: collName, index: "a_1", readConcern: snapshotReadConcern}),
    ErrorCodes.InvalidOptions);

rst.stopSet();
}());
