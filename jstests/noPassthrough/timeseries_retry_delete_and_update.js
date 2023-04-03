/**
 * Tests retrying of time-series delete and update operations that are eligible for retryable writes
 * (specifically single deletes and updates).
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
 *   featureFlagTimeseriesDeletesSupport,
 * ]
 */
(function() {
'use strict';

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const timeFieldName = 'time';
const metaFieldName = 'tag';
const dateTime = ISODate("2021-07-12T16:00:00Z");
let collCount = 0;

let retriedCommandsCount = 0;
let retriedStatementsCount = 0;

/**
 * Verifies that a timeseries delete or update command supports retryable writes. The arguments to
 * this function are an array of documents to insert initially, a command builder function that
 * returns the command object given the collection to run it on, and a validate function that
 * validates the result after the command has been applied to the collection.
 */
const runTest = function(initialDocs, cmdBuilderFn, validateFn) {
    const session = primary.startSession({retryWrites: true});
    const testDB = session.getDatabase(jsTestName());

    const coll = testDB.getCollection('timeseres_retry_delete_and_update_' + collCount++);
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    assert.commandWorked(testDB.runCommand({
        insert: coll.getName(),
        documents: initialDocs,
        lsid: session.getSessionId(),
        txnNumber: NumberLong(0)
    }));

    // For retryable writes, the server uses 'txnNumber' as the key to look up previously executed
    // operations in the sesssion.
    let cmdObj = cmdBuilderFn(coll);
    cmdObj["lsid"] = session.getSessionId();
    cmdObj["txnNumber"] = NumberLong(1);

    assert.commandWorked(testDB.runCommand(cmdObj), 'Failed to write bucket on first write');
    assert.commandWorked(testDB.runCommand(cmdObj), 'Failed to write bucket on retry write');

    validateFn(coll);

    const transactionsServerStatus = testDB.serverStatus().transactions;
    assert.eq(++retriedCommandsCount,
              transactionsServerStatus.retriedCommandsCount,
              'Incorrect statistic in db.serverStatus(): ' + tojson(transactionsServerStatus));
    assert.eq(++retriedStatementsCount,
              transactionsServerStatus.retriedStatementsCount,
              'Incorrect statistic in db.serverStatus(): ' + tojson(transactionsServerStatus));

    session.endSession();
};

const allDocumentsSameBucket = [
    {[timeFieldName]: dateTime, [metaFieldName]: "A", f: 100},
    {[timeFieldName]: dateTime, [metaFieldName]: "A", f: 101},
    {[timeFieldName]: dateTime, [metaFieldName]: "A", f: 102},
];
const allDocumentsDifferentBuckets = [
    {[timeFieldName]: dateTime, [metaFieldName]: "A", f: 100},
    {[timeFieldName]: dateTime, [metaFieldName]: "B", f: 101},
    {[timeFieldName]: dateTime, [metaFieldName]: "C", f: 102},
];

function deleteCmdBuilderFn(coll) {
    return {delete: coll.getName(), deletes: [{q: {}, limit: 1}]};
}
function deleteValidateFn(coll) {
    assert.eq(coll.countDocuments({}), 2, "Expected exactly one document to be deleted.");
}

(function testPartialBucketDelete() {
    runTest(allDocumentsSameBucket, deleteCmdBuilderFn, deleteValidateFn);
})();
(function testFullBucketDelete() {
    runTest(allDocumentsDifferentBuckets, deleteCmdBuilderFn, deleteValidateFn);
})();

function updateCmdBuilderFn(coll) {
    return {update: coll.getName(), updates: [{q: {}, u: {$inc: {updated: 1}}, multi: false}]};
}
function updateValidateFn(coll) {
    assert.eq(coll.countDocuments({updated: {$exists: true}}),
              1,
              "Expected exactly one document to be updated.");
    assert.eq(coll.countDocuments({updated: 1}), 1, "Expected document to be updated only once.");
}

// TODO SERVER-73726 Enable update tests.
// (function testPartialBucketUpdate() {
//     runTest(allDocumentsSameBucket, updateCmdBuilderFn, updateValidateFn);
// })();
// (function testFullBucketUpdate() {
//     runTest(allDocumentsDifferentBuckets, updateCmdBuilderFn, updateValidateFn);
// })();

rst.stopSet();
})();
