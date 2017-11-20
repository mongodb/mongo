/**
 * Tests that a retryable write started on one primary can be continued on a different node after
 * failover.
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    function stepDownPrimary(replTest) {
        let exception = assert.throws(function() {
            let res = assert.commandWorked(
                replTest.getPrimary().adminCommand({replSetStepDown: 10, force: true}));
            print("replSetStepDown did not throw exception but returned: " + tojson(res));
        });
        assert(isNetworkError(exception),
               "replSetStepDown did not disconnect client; failed with " + tojson(exception));
    }

    const replTest = new ReplSetTest({nodes: 3});
    replTest.startSet();
    replTest.initiate();

    ////////////////////////////////////////////////////////////////////////
    // Test insert command

    let insertCmd = {
        insert: "foo",
        documents: [{_id: 10}, {_id: 30}],
        ordered: false,
        lsid: {id: UUID()},
        txnNumber: NumberLong(5)
    };

    // Run the command on the primary and wait for replication.
    let primary = replTest.getPrimary();
    let testDB = primary.getDB("test");

    let result = assert.commandWorked(testDB.runCommand(insertCmd));
    assert.eq(2, testDB.foo.find().itcount());

    replTest.awaitReplication();

    // Step down the primary and wait for a new one.
    stepDownPrimary(replTest);

    let newPrimary = replTest.getPrimary();
    testDB = newPrimary.getDB("test");

    let oplog = newPrimary.getDB("local").oplog.rs;
    let insertOplogEntries = oplog.find({ns: "test.foo", op: "i"}).itcount();

    // Retry the command on the secondary and verify it wasn't repeated.
    let retryResult = assert.commandWorked(testDB.runCommand(insertCmd));
    assert.eq(result.ok, retryResult.ok);
    assert.eq(result.n, retryResult.n);
    assert.eq(result.writeErrors, retryResult.writeErrors);
    assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);

    assert.eq(2, testDB.foo.find().itcount());

    assert.eq(insertOplogEntries, oplog.find({ns: "test.foo", op: "i"}).itcount());

    ////////////////////////////////////////////////////////////////////////
    // Test update command

    let updateCmd = {
        update: "foo",
        updates: [
            {q: {_id: 10}, u: {$inc: {x: 1}}},  // in place
            {q: {_id: 20}, u: {$inc: {y: 1}}, upsert: true},
            {q: {_id: 30}, u: {z: 1}}  // replacement
        ],
        ordered: false,
        lsid: {id: UUID()},
        txnNumber: NumberLong(10),
    };

    primary = replTest.getPrimary();
    testDB = primary.getDB("test");

    // Run the command on the primary and wait for replication.
    result = assert.commandWorked(testDB.runCommand(updateCmd));
    assert.eq(3, testDB.foo.find().itcount());

    replTest.awaitReplication();

    // Step down the primary and wait for a new one.
    stepDownPrimary(replTest);

    newPrimary = replTest.getPrimary();
    testDB = newPrimary.getDB("test");

    oplog = newPrimary.getDB("local").oplog.rs;
    let updateOplogEntries = oplog.find({ns: "test.foo", op: "u"}).itcount();

    // Upserts are stored as inserts if they match no existing documents.
    insertOplogEntries = oplog.find({ns: "test.foo", op: "i"}).itcount();

    // Retry the command on the secondary and verify it wasn't repeated.
    retryResult = assert.commandWorked(testDB.runCommand(updateCmd));
    assert.eq(result.ok, retryResult.ok);
    assert.eq(result.n, retryResult.n);
    assert.eq(result.nModified, retryResult.nModified);
    assert.eq(result.upserted, retryResult.upserted);
    assert.eq(result.writeErrors, retryResult.writeErrors);
    assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);

    assert.eq(3, testDB.foo.find().itcount());

    assert.eq({_id: 10, x: 1}, testDB.foo.findOne({_id: 10}));
    assert.eq({_id: 20, y: 1}, testDB.foo.findOne({_id: 20}));
    assert.eq({_id: 30, z: 1}, testDB.foo.findOne({_id: 30}));

    assert.eq(updateOplogEntries, oplog.find({ns: "test.foo", op: "u"}).itcount());
    assert.eq(insertOplogEntries, oplog.find({ns: "test.foo", op: "i"}).itcount());

    ////////////////////////////////////////////////////////////////////////
    // Test delete command

    let deleteCmd = {
        delete: "foo",
        deletes: [{q: {x: 1}, limit: 1}, {q: {y: 1}, limit: 1}],
        ordered: false,
        lsid: {id: UUID()},
        txnNumber: NumberLong(15),
    };

    primary = replTest.getPrimary();
    testDB = primary.getDB("test");

    assert.writeOK(testDB.foo.insert({_id: 40, x: 1}));
    assert.writeOK(testDB.foo.insert({_id: 50, y: 1}));

    // Run the command on the primary and wait for replication.
    result = assert.commandWorked(testDB.runCommand(deleteCmd));
    assert.eq(1, testDB.foo.find({x: 1}).itcount());
    assert.eq(1, testDB.foo.find({y: 1}).itcount());

    replTest.awaitReplication();

    // Step down the primary and wait for a new one.
    stepDownPrimary(replTest);

    newPrimary = replTest.getPrimary();
    testDB = newPrimary.getDB("test");

    oplog = newPrimary.getDB("local").oplog.rs;
    let deleteOplogEntries = oplog.find({ns: "test.foo", op: "d"}).itcount();

    // Retry the command on the secondary and verify it wasn't repeated.
    retryResult = assert.commandWorked(testDB.runCommand(deleteCmd));
    assert.eq(result.ok, retryResult.ok);
    assert.eq(result.n, retryResult.n);
    assert.eq(result.writeErrors, retryResult.writeErrors);
    assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);

    assert.eq(1, testDB.foo.find({x: 1}).itcount());
    assert.eq(1, testDB.foo.find({y: 1}).itcount());

    assert.eq(deleteOplogEntries, oplog.find({ns: "test.foo", op: "d"}).itcount());

    replTest.stopSet();
})();
