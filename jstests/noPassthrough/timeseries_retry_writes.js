/**
 * Tests retrying of time-series insert operations.
 * @tags: [
 *     requires_replication,
 *     sbe_incompatible,
 * ]
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');

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
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
if (!TimeseriesTest.timeseriesCollectionsEnabled(primary)) {
    jsTestLog('Skipping test because the time-series collection feature flag is disabled');
    rst.stopSet();
    return;
}

const timeFieldName = 'time';
let collCount = 0;

/**
 * Accepts three measurements. The first measurement is used to create a new bucket. The second and
 * third measurements are used to appendto the bucket that was just created. We should see one
 * bucket created in the time-series collection.
 */
const runTest = function(docInsert, docUpdateA, docUpdateB) {
    const session = primary.startSession({retryWrites: true});
    const testDB = session.getDatabase('test');

    const coll = testDB.getCollection('t_' + collCount++);
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    jsTestLog('Running test: collection: ' + coll.getFullName() + '; bucket collection: ' +
              bucketsColl.getFullName() + '; initial measurement: ' + tojson(docInsert) +
              '; measurement to append A: ' + tojson(docUpdateA) +
              '; measurement to append B: ' + tojson(docUpdateB));

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

    // For retryable writes, the server uses 'txnNumber' as the key to look up previously executed
    // operations in the sesssion.
    assert.commandWorked(
        testDB.runCommand({
            insert: coll.getName(),
            documents: [docInsert],
            lsid: session.getSessionId(),
            txnNumber: NumberLong(0),
        }),
        'failed to create bucket with initial doc (first write): ' + tojson(docInsert));
    assert.commandWorked(
        testDB.runCommand({
            insert: coll.getName(),
            documents: [docInsert],
            lsid: session.getSessionId(),
            txnNumber: NumberLong(0),
        }),
        'failed to create bucket with initial doc (retry write): ' + tojson(docInsert));

    assert.commandWorked(testDB.runCommand({
        insert: coll.getName(),
        documents: [docUpdateA],
        lsid: session.getSessionId(),
        txnNumber: NumberLong(1),
    }),
                         'failed to append doc A to bucket (first write): ' + tojson(docUpdateA));
    assert.commandWorked(testDB.runCommand({
        insert: coll.getName(),
        documents: [docUpdateA],
        lsid: session.getSessionId(),
        txnNumber: NumberLong(1),
    }),
                         'failed to append doc A to bucket (retry write): ' + tojson(docUpdateA));

    assert.commandWorked(testDB.runCommand({
        insert: coll.getName(),
        documents: [docUpdateB],
        lsid: session.getSessionId(),
        txnNumber: NumberLong(2),
    }),
                         'failed to append doc B to bucket (first write): ' + tojson(docUpdateB));
    assert.commandWorked(testDB.runCommand({
        insert: coll.getName(),
        documents: [docUpdateB],
        lsid: session.getSessionId(),
        txnNumber: NumberLong(2),
    }),
                         'failed to append doc B to bucket (retry write): ' + tojson(docUpdateB));

    // Retryable writes on time-series collections with more than one measurement are not allowed.
    let docs = [docInsert, docUpdateA, docUpdateB];
    assert.commandFailedWithCode(testDB.runCommand({
        insert: coll.getName(),
        documents: docs,
        lsid: session.getSessionId(),
        txnNumber: NumberLong(3),
    }),
                                 ErrorCodes.OperationFailed);

    // This test case ensures that the batch size error handling is consistent with non-time-series
    // collections.
    assert.commandFailedWithCode(testDB.runCommand({
        insert: coll.getName(),
        documents: [],  // No documents
        lsid: session.getSessionId(),
        txnNumber: NumberLong(4),
    }),
                                 ErrorCodes.InvalidLength);

    // Check view.
    const viewDocs = coll.find({}).sort({_id: 1}).toArray();
    assert.eq(docs.length, viewDocs.length, viewDocs);
    for (let i = 0; i < docs.length; i++) {
        assert.docEq(docs[i], viewDocs[i], 'unexpected doc from view: ' + i);
    }

    // Check bucket collection.
    const bucketDocs = bucketsColl.find().sort({_id: 1}).toArray();
    assert.eq(1, bucketDocs.length, bucketDocs);

    const bucketDoc = bucketDocs[0];
    jsTestLog('Bucket for test collection: ' + coll.getFullName() +
              ': bucket collection: ' + bucketsColl.getFullName() + ': ' + tojson(bucketDoc));

    // Check bucket.
    assert.eq(docs.length,
              Object.keys(bucketDoc.data[timeFieldName]).length,
              'invalid number of measurements in first bucket: ' + tojson(bucketDoc));

    // Keys in data field should match element indexes in 'docs' array.
    for (let i = 0; i < docs.length; i++) {
        assert(bucketDoc.data[timeFieldName].hasOwnProperty(i.toString()),
               'missing element for index ' + i + ' in data field: ' + tojson(bucketDoc));
        assert.eq(docs[i][timeFieldName],
                  bucketDoc.data[timeFieldName][i.toString()],
                  'invalid time for measurement ' + i + ' in data field: ' + tojson(bucketDoc));
    }

    const transactionsServerStatus = testDB.serverStatus().transactions;
    assert.eq(docs.length,
              transactionsServerStatus.retriedCommandsCount,
              'Incorrect statistic in db.serverStatus(): ' + tojson(transactionsServerStatus));
    assert.eq(docs.length,
              transactionsServerStatus.retriedStatementsCount,
              'Incorrect statistic in db.serverStatus(): ' + tojson(transactionsServerStatus));

    session.endSession();
};

const t = [
    ISODate("2021-01-20T00:00:00.000Z"),
    ISODate("2021-01-20T00:10:00.000Z"),
    ISODate("2021-01-20T00:20:00.000Z")
];

// One measurement per write operation.
runTest({_id: 0, time: t[0], x: 0}, {_id: 1, time: t[1], x: 1}, {_id: 2, time: t[2], x: 2});

rst.stopSet();
})();
