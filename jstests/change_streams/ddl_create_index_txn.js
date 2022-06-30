/**
 * Tests that a change stream will correctly unwind createIndexes operations from applyOps when
 * createIndexes is done in a transaction.
 *
 * @tags: [
 *     uses_transactions,
 *     requires_majority_read_concern,
 *     requires_fcv_60,
 *     # In order to run this test with sharding we would have to create a transaction that creates
 *     # the collection, shards it, and then creates the index. however sharding a collection in a
 *     # transaction is not allowed and creating an index in a transaction on a collection that was
 *     # not created in that transaction is also not allowed. so this test only works with unsharded
 *     # collections.
 *     assumes_unsharded_collection
 * ]
 */
(function() {

load("jstests/libs/auto_retry_transaction_in_sharding.js");  // For withTxnAndAutoRetryOnMongos.
load("jstests/libs/change_stream_util.js");                  // For ChangeStreamTest.
load("jstests/libs/fixture_helpers.js");                     // For FixtureHelpers.isMongos.
load('jstests/libs/collection_drop_recreate.js');            // 'assertDropCollection'.

const dbName = jsTestName() + "_db0";
const collName = jsTestName() + '_1';
const otherCollName = jsTestName() + "_2";
const coll = db.getSiblingDB(dbName)[jsTestName()];

const otherDBName = jsTestName() + "_3";
const otherDB = db.getSiblingDB(otherDBName);
const otherDBCollName = "someColl";

const session = db.getMongo().startSession();

const sessionDB = session.getDatabase(dbName);
const sessionOtherDB = session.getDatabase(otherDBName);
const sessionColl = sessionDB[collName];
const sessionOtherColl = sessionDB[otherCollName];
const sessionOtherDBColl = sessionOtherDB[otherDBCollName];

assertDropCollection(sessionDB, collName);
assertDropCollection(sessionDB, otherCollName);
assertDropCollection(sessionOtherDB, otherDBCollName);

let csOptions = {showExpandedEvents: true};
const pipeline = [{$changeStream: csOptions}, {$project: {"lsid.uid": 0}}];

let cst = new ChangeStreamTest(sessionDB);
let changeStream = cst.startWatchingChanges({pipeline, collection: collName});

const testStartTime = changeStream.postBatchResumeToken;
assert.neq(testStartTime, undefined);

const txnOptions = {
    readConcern: {level: "local"},
    writeConcern: {w: "majority"}
};

withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionColl.createIndex({unused: 1}));
}, txnOptions);

const lsid = session.getSessionId();
const txnNumberColl = session.getTxnNumber_forTesting();

withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionOtherColl.createIndex({unused: 1}));
}, txnOptions);

const txnNumberOtherColl = session.getTxnNumber_forTesting();

withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionOtherDBColl.createIndex({unused: 1}));
}, txnOptions);

const txnNumberOtherDBColl = session.getTxnNumber_forTesting();

const expectedChanges = [
    {operationType: "create", ns: {db: dbName, coll: collName}},
    {
        operationType: "createIndexes",
        ns: {db: dbName, coll: collName},
        "operationDescription": {"indexes": [{"v": 2, "key": {"unused": 1}, "name": "unused_1"}]},
        lsid,
        txnNumber: txnNumberColl
    }
];

// Test single coll changeStream.
cst.assertNextChangesEqual({cursor: changeStream, expectedChanges});

// Test whole db changeStream.
const otherCollEvents = [
    {operationType: "create", ns: {db: dbName, coll: otherCollName}},
    {
        operationType: "createIndexes",
        ns: {db: dbName, coll: otherCollName},
        "operationDescription": {"indexes": [{"v": 2, "key": {"unused": 1}, "name": "unused_1"}]},
        lsid,
        txnNumber: txnNumberOtherColl
    }
];
expectedChanges.push(...otherCollEvents);
csOptions.startAfter = testStartTime;
changeStream = cst.startWatchingChanges({pipeline, collection: 1});
cst.assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges});

cst.cleanUp();
cst = new ChangeStreamTest(db.getSiblingDB("admin"));

// Test whole cluster changeStream.
const otherDBEvents = [
    {operationType: "create", ns: {db: otherDBName, coll: otherDBCollName}},
    {
        operationType: "createIndexes",
        ns: {db: otherDBName, coll: otherDBCollName},
        "operationDescription": {"indexes": [{"v": 2, "key": {"unused": 1}, "name": "unused_1"}]},
        lsid,
        txnNumber: txnNumberOtherDBColl
    }
];
expectedChanges.push(...otherDBEvents);
csOptions.allChangesForCluster = true;
changeStream = cst.startWatchingChanges({pipeline, collection: 1});
cst.assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges});

cst.cleanUp();
})();
