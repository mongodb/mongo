// @tags: [requires_replication, uses_transactions, uses_atclustertime]

// Test the correct timestamping of insert, update, and delete writes along with their accompanying
// index updates.
//
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const testDB = rst.getPrimary().getDB(dbName);
const coll = testDB.getCollection(collName);

// For consistent results, generate a single delete (applyOps) batch.
assert.commandWorked(testDB.adminCommand({setParameter: 1, batchedDeletesTargetBatchTimeMS: 0}));
assert.commandWorked(testDB.adminCommand({setParameter: 1, batchedDeletesTargetStagedDocBytes: 0}));
assert.commandWorked(testDB.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: 0}));

if (!testDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
    rst.stopSet();
    quit();
}

// Turn off timestamp reaping.
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
    mode: "alwaysOn",
}));

const session = testDB.getMongo().startSession({causalConsistency: false});
const sessionDb = session.getDatabase(dbName);
const response = assert.commandWorked(testDB.createCollection("coll"));
const startTime = response.operationTime;

function check(atClusterTime, expected) {
    session.startTransaction({readConcern: {level: "snapshot", atClusterTime: atClusterTime}});
    // Check both a collection scan and scanning the _id index.
    [{$natural: 1}, {_id: 1}].forEach(sort => {
        let response = assert.commandWorked(
            sessionDb.runCommand({find: collName, sort: sort, singleBatch: true}));
        assert.eq(expected, response.cursor.firstBatch);
    });
    assert.commandWorked(session.commitTransaction_forTesting());
}

// insert

let request = {insert: coll.getName(), documents: [{_id: 1}], ordered: false};
assert.commandWorked(coll.runCommand(request));
request = {
    insert: coll.getName(),
    documents: [{_id: 2}],
    ordered: false
};
assert.commandWorked(coll.runCommand(request));

const oplog = rst.getPrimary().getDB("local").getCollection("oplog.rs");
let ts1 = oplog.findOne({o: {_id: 1}}).ts;
let ts2 = oplog.findOne({o: {_id: 2}}).ts;

check(startTime, []);
check(ts1, [{_id: 1}]);
check(ts2, [{_id: 1}, {_id: 2}]);

// upsert

request = {
    update: coll.getName(),
    updates: [
        {q: {_id: 3, a: 1}, u: {$set: {a: 2}}, upsert: true},
        {q: {_id: 4, a: 1}, u: {$set: {a: 3}}, upsert: true}
    ],
    ordered: true
};
assert.commandWorked(coll.runCommand(request));

ts1 = oplog.findOne({o: {_id: 3, a: 2}}).ts;
ts2 = oplog.findOne({o: {_id: 4, a: 3}}).ts;

check(ts1, [{_id: 1}, {_id: 2}, {_id: 3, a: 2}]);
check(ts2, [{_id: 1}, {_id: 2}, {_id: 3, a: 2}, {_id: 4, a: 3}]);

// update

request = {
    update: coll.getName(),
    updates: [{q: {_id: 3, a: 2}, u: {$set: {a: 4}}}, {q: {_id: 4, a: 3}, u: {$set: {a: 5}}}],
    ordered: true
};
assert.commandWorked(coll.runCommand(request));

ts1 = oplog.findOne({op: 'u', o2: {_id: 3}}).ts;
ts2 = oplog.findOne({op: 'u', o2: {_id: 4}}).ts;

check(ts1, [{_id: 1}, {_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 3}]);
check(ts2, [{_id: 1}, {_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 5}]);

// Multi insert
request = {
    insert: coll.getName(),
    documents: [{_id: 5}, {_id: 6}, {_id: 7}],
    ordered: false
};
if (FeatureFlagUtil.isEnabled(testDB, "ReplicateVectoredInsertsTransactionally")) {
    assert.commandWorked(testDB.adminCommand({setParameter: 1, internalInsertMaxBatchSize: 2}));
    assert.commandWorked(coll.runCommand(request));
    // First two inserts will be in an applyOps, last will be on its own.
    ts1 = oplog.findOne({"o.applyOps.o": {_id: 5}}).ts;
    ts2 = oplog.findOne({o: {_id: 7}}).ts;

    check(startTime, []);
    check(ts1, [{_id: 1}, {_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 5}, {_id: 5}, {_id: 6}]);
    check(ts2, [{_id: 1}, {_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 5}, {_id: 5}, {_id: 6}, {_id: 7}]);
} else {
    assert.commandWorked(coll.runCommand(request));
    ts1 = oplog.findOne({o: {_id: 5}}).ts;
    ts2 = oplog.findOne({o: {_id: 6}}).ts;
    let ts3 = oplog.findOne({o: {_id: 7}}).ts;

    check(startTime, []);
    check(ts1, [{_id: 1}, {_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 5}, {_id: 5}]);
    check(ts2, [{_id: 1}, {_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 5}, {_id: 5}, {_id: 6}]);
    check(ts3, [{_id: 1}, {_id: 2}, {_id: 3, a: 4}, {_id: 4, a: 5}, {_id: 5}, {_id: 6}, {_id: 7}]);
}

// delete

request = {
    delete: coll.getName(),
    deletes: [{q: {}, limit: 0}],
    ordered: false
};

assert.commandWorked(coll.runCommand(request));

const applyOps = oplog.findOne({op: 'c', ns: 'admin.$cmd', 'o.applyOps.op': 'd'});
const ts = applyOps['ts'];
check(ts, []);

session.endSession();
rst.stopSet();
