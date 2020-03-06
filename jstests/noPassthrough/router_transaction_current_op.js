// Verifies currentOp returns the expected fields for idle and active transactions in basic cases.
// More cases are covered in unit tests.
// @tags: [uses_transactions, uses_atclustertime]
(function() {
"use strict";

load("jstests/libs/parallelTester.js");  // for Thread.

function verifyCurrentOpFields(res, isActive) {
    // Verify top level fields relevant to transactions. Note this does not include every field, so
    // the number of fields in the response shouldn't be asserted on.

    const expectedFields = [
        "type",
        "host",
        "desc",
        "connectionId",
        "client",
        "appName",
        "clientMetadata",
        "active",
        "lsid",
        "transaction",
    ];

    assert.hasFields(res, expectedFields, tojson(res));

    if (isActive) {
        assert.eq(res.type, "op", tojson(res));
    } else {
        assert.eq(res.type, "idleSession", tojson(res));
        assert.eq(res.desc, "inactive transaction", tojson(res));
    }

    // Verify the transactions sub object.

    const transaction = res.transaction;
    const expectedTransactionsFields = [
        "parameters",
        "startWallClockTime",
        "timeOpenMicros",
        "timeActiveMicros",
        "timeInactiveMicros",
        "globalReadTimestamp",
        "numParticipants",
        "participants",
        "numNonReadOnlyParticipants",
        "numReadOnlyParticipants",
        // Commit hasn't started so don't expect 'commitStartWallClockTime' or 'commitType'.
    ];

    assert.hasFields(transaction, expectedTransactionsFields, tojson(transaction));
    assert.eq(
        expectedTransactionsFields.length, Object.keys(transaction).length, tojson(transaction));

    // Verify transaction parameters sub object.

    const parameters = transaction.parameters;
    const expectedParametersFields = [
        "txnNumber",
        "autocommit",
        "readConcern",
    ];

    assert.hasFields(parameters, expectedParametersFields, tojson(parameters));
    assert.eq(expectedParametersFields.length, Object.keys(parameters).length, tojson(parameters));

    // Verify participants sub array.

    const participants = transaction.participants;
    const expectedParticipantFields = [
        "name",
        "coordinator",
        // 'readOnly' will not be set until a response has been received from that participant, so
        // it will not be present for the active transaction because of the failpoint and is handled
        // specially.
    ];

    participants.forEach((participant) => {
        assert.hasFields(participant, expectedParticipantFields, tojson(participant));
        if (isActive) {
            // 'readOnly' should not be set.
            assert.eq(expectedParticipantFields.length,
                      Object.keys(participant).length,
                      tojson(participant));
        } else {
            // 'readOnly' should always be set for the inactive transaction.
            assert.hasFields(participant, ["readOnly"], tojson(participant));
            assert.eq(expectedParticipantFields.length + 1,  // +1 for readOnly.
                      Object.keys(participant).length,
                      tojson(participant));
        }
    });
}

function getCurrentOpForFilter(st, matchFilter) {
    const res = st.s.getDB("admin")
                    .aggregate([{$currentOp: {localOps: true}}, {$match: matchFilter}])
                    .toArray();
    assert.eq(1, res.length, res);
    return res[0];
}

const dbName = "test";
const collName = "foo";
const st = new ShardingTest({shards: 1, config: 1});

const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);

// Insert a document to set up a collection.
assert.commandWorked(sessionDB[collName].insert({x: 1}));

jsTest.log("Inactive transaction.");
(() => {
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.eq(1, sessionDB[collName].find({x: 1}).itcount());

    const res = getCurrentOpForFilter(st, {"lsid.id": session.getSessionId().id});
    verifyCurrentOpFields(res, false /* isActive */);

    assert.commandWorked(session.abortTransaction_forTesting());
})();

jsTest.log("Active transaction.");
(() => {
    assert.commandWorked(st.rs0.getPrimary().adminCommand(
        {configureFailPoint: "waitInFindBeforeMakingBatch", mode: "alwaysOn"}));

    const txnThread = new Thread(function(host, dbName, collName) {
        const mongosConn = new Mongo(host);
        const threadSession = mongosConn.startSession();

        threadSession.startTransaction({readConcern: {level: "snapshot"}});
        assert.commandWorked(threadSession.getDatabase(dbName).runCommand(
            {find: collName, filter: {}, comment: "active_txn_find"}));

        assert.commandWorked(threadSession.abortTransaction_forTesting());
        threadSession.endSession();
    }, st.s.host, dbName, collName);
    txnThread.start();

    // Wait until we know the failpoint has been reached.
    assert.soon(function() {
        const filter = {"failpointMsg": "waitInFindBeforeMakingBatch"};
        return assert.commandWorked(st.rs0.getPrimary().getDB("admin").currentOp(filter))
                   .inprog.length === 1;
    });

    // We don't know the id of the session started by the parallel thread, so use the find's comment
    // to get its currentOp output.
    const res = getCurrentOpForFilter(st, {"command.comment": "active_txn_find"});
    verifyCurrentOpFields(res, true /* isActive */);

    assert.commandWorked(st.rs0.getPrimary().adminCommand(
        {configureFailPoint: "waitInFindBeforeMakingBatch", mode: "off"}));
    txnThread.join();
})();

session.endSession();
st.stop();
}());
