/**
 * Validates the operationTime value in the command response depends on the read/writeConcern of the
 * the read/write commmand that produced it.
 */
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");           // For startSetIfSupportsReadMajority.
    load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries,
                                                 // restartReplicationOnSecondaries
    var name = "operation_time_read_and_write_concern";

    var replTest = new ReplSetTest(
        {name: name, nodes: 3, nodeOptions: {enableMajorityReadConcern: ""}, waitForKeys: true});

    if (!startSetIfSupportsReadMajority(replTest)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }
    replTest.initiate();

    var res;
    var testDB = replTest.getPrimary().getDB(name);
    var collectionName = "foo";

    // readConcern level majority:
    // operationTime is the cluster time of the last committed op in the oplog.
    jsTestLog("Testing operationTime for readConcern level majority with afterClusterTime.");
    var majorityDoc = {_id: 10, x: 1};
    var localDoc = {_id: 15, x: 2};

    res = assert.commandWorked(testDB.runCommand(
        {insert: collectionName, documents: [majorityDoc], writeConcern: {w: "majority"}}));
    var majorityWriteOperationTime = res.operationTime;

    stopReplicationOnSecondaries(replTest);

    res = assert.commandWorked(
        testDB.runCommand({insert: collectionName, documents: [localDoc], writeConcern: {w: 1}}));
    var localWriteOperationTime = res.operationTime;

    assert(localWriteOperationTime > majorityWriteOperationTime);

    res = assert.commandWorked(testDB.runCommand({
        find: collectionName,
        readConcern: {level: "majority", afterClusterTime: majorityWriteOperationTime}
    }));
    var majorityReadOperationTime = res.operationTime;

    assert.eq(res.cursor.firstBatch,
              [majorityDoc],
              "only the committed document, " + tojson(majorityDoc) +
                  ", should be returned for the majority read with afterClusterTime: " +
                  majorityWriteOperationTime);
    assert.eq(majorityReadOperationTime,
              majorityWriteOperationTime,
              "the operationTime of the majority read, " + majorityReadOperationTime +
                  ", should be the cluster time of the last committed op in the oplog, " +
                  majorityWriteOperationTime);

    // Validate that after replication, the local write data is now returned by the same query.
    restartReplicationOnSecondaries(replTest);
    replTest.awaitLastOpCommitted();

    res = assert.commandWorked(testDB.runCommand({
        find: collectionName,
        sort: {_id: 1},  // So the order of the documents is defined for testing.
        readConcern: {level: "majority", afterClusterTime: majorityWriteOperationTime}
    }));
    var secondMajorityReadOperationTime = res.operationTime;

    assert.eq(res.cursor.firstBatch,
              [majorityDoc, localDoc],
              "expected both inserted documents, " + tojson([majorityDoc, localDoc]) +
                  ", to be returned for the second majority read with afterClusterTime: " +
                  majorityWriteOperationTime);
    assert.eq(secondMajorityReadOperationTime,
              localWriteOperationTime,
              "the operationTime of the second majority read, " + secondMajorityReadOperationTime +
                  ", should be the cluster time of the replicated local write, " +
                  localWriteOperationTime);

    // readConcern level linearizable is not currently supported.
    jsTestLog("Verifying readConcern linearizable with afterClusterTime is not supported.");
    res = assert.commandFailedWithCode(
        testDB.runCommand({
            find: collectionName,
            filter: localDoc,
            readConcern: {level: "linearizable", afterClusterTime: majorityReadOperationTime}
        }),
        ErrorCodes.InvalidOptions,
        "linearizable reads with afterClusterTime are not supported and should not be allowed");

    // writeConcern level majority:
    // operationTime is the cluster time of the write if it succeeds, or of the previous successful
    // write at the time the write was determined to have failed, or a no-op.
    jsTestLog("Testing operationTime for writeConcern level majority.");
    var successfulDoc = {_id: 1000, y: 1};
    var failedDoc = {_id: 1000, y: 2};

    res = assert.commandWorked(testDB.runCommand(
        {insert: collectionName, documents: [successfulDoc], writeConcern: {w: "majority"}}));
    var majorityWriteOperationTime = res.operationTime;

    stopReplicationOnSecondaries(replTest);

    res = testDB.runCommand({
        insert: collectionName,
        documents: [failedDoc],
        writeConcern: {w: "majority", wtimeout: 1000}
    });
    assert.eq(res.writeErrors[0].code, ErrorCodes.DuplicateKey);
    var failedWriteOperationTime = res.operationTime;

    assert.eq(
        failedWriteOperationTime,
        majorityWriteOperationTime,
        "the operationTime of the failed majority write, " + failedWriteOperationTime +
            ", should be the cluster time of the last successful write at the time it failed, " +
            majorityWriteOperationTime);
    replTest.stopSet();
})();
