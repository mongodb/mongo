/*
 * Test that internal sessions are only supported in FCV latest.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions]
 */
(function() {
'use strict';

TestData.disableImplicitSessions = true;

const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: {maxSessions: 1}}});

rst.startSet();
rst.initiate();

const kDbName = "testDb";
const kCollName = "testColl";
const primary = rst.getPrimary();
const testDB = primary.getDB(kDbName);

const sessionUUID = UUID();
const lsid0 = {
    id: sessionUUID,
    txnNumber: NumberLong(35),
    stmtId: NumberInt(0)
};
const txnNumber0 = NumberLong(0);
const lsid1 = {
    id: sessionUUID,
    txnUUID: UUID()
};
const txnNumber1 = NumberLong(35);

assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

assert.commandFailedWithCode(testDB.runCommand({
    insert: kCollName,
    documents: [{x: 0}],
    lsid: lsid0,
    txnNumber: txnNumber0,
    startTransaction: true,
    autocommit: false
}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(testDB.runCommand({
    insert: kCollName,
    documents: [{x: 1}],
    lsid: lsid1,
    txnNumber: txnNumber1,
    startTransaction: true,
    autocommit: false
}),
                             ErrorCodes.InvalidOptions);

assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

assert.commandWorked(testDB.runCommand({
    insert: kCollName,
    documents: [{x: 0}],
    lsid: lsid0,
    txnNumber: txnNumber0,
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(testDB.adminCommand(
    {commitTransaction: 1, lsid: lsid0, txnNumber: txnNumber0, autocommit: false}));

assert.commandWorked(testDB.runCommand({
    insert: kCollName,
    documents: [{x: 1}],
    lsid: lsid1,
    txnNumber: txnNumber1,
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(testDB.adminCommand(
    {commitTransaction: 1, lsid: lsid1, txnNumber: txnNumber1, autocommit: false}));

rst.stopSet();
})();
