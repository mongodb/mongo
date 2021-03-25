/**
 * Tests retrying of time-series insert operations.
 * @tags: [
 *   requires_replication,
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

let retriedCommandsCount = 0;
let retriedStatementsCount = 0;

/**
 * Accepts three arrays of measurements. The first set of measurements is used to create a new
 * bucket. The second and third sets of measurements are used to append to the bucket that was just
 * created. We should see one bucket created in the time-series collection.
 */
const runTest = function(docsInsert, docsUpdateA, docsUpdateB) {
    const session = primary.startSession({retryWrites: true});
    const testDB = session.getDatabase('test');

    const coll = testDB.getCollection('t_' + collCount++);
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    jsTestLog('Running test: collection: ' + coll.getFullName() + '; bucket collection: ' +
              bucketsColl.getFullName() + '; initial measurements: ' + tojson(docsInsert) +
              '; measurements to append A: ' + tojson(docsUpdateA) +
              '; measurements to append B: ' + tojson(docsUpdateB));

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

    // For retryable writes, the server uses 'txnNumber' as the key to look up previously executed
    // operations in the sesssion.
    assert.commandWorked(
        testDB.runCommand({
            insert: coll.getName(),
            documents: docsInsert,
            lsid: session.getSessionId(),
            txnNumber: NumberLong(0),
        }),
        'failed to create bucket with initial docs (first write): ' + tojson(docsInsert));
    assert.commandWorked(
        testDB.runCommand({
            insert: coll.getName(),
            documents: docsInsert,
            lsid: session.getSessionId(),
            txnNumber: NumberLong(0),
        }),
        'failed to create bucket with initial docs (retry write): ' + tojson(docsInsert));

    assert.commandWorked(testDB.runCommand({
        insert: coll.getName(),
        documents: docsUpdateA,
        lsid: session.getSessionId(),
        txnNumber: NumberLong(1),
    }),
                         'failed to append docs A to bucket (first write): ' + tojson(docsUpdateA));
    assert.commandWorked(testDB.runCommand({
        insert: coll.getName(),
        documents: docsUpdateA,
        lsid: session.getSessionId(),
        txnNumber: NumberLong(1),
    }),
                         'failed to append docs A to bucket (retry write): ' + tojson(docsUpdateA));

    assert.commandWorked(testDB.runCommand({
        insert: coll.getName(),
        documents: docsUpdateB,
        lsid: session.getSessionId(),
        txnNumber: NumberLong(2),
    }),
                         'failed to append docs B to bucket (first write): ' + tojson(docsUpdateB));
    assert.commandWorked(testDB.runCommand({
        insert: coll.getName(),
        documents: docsUpdateB,
        lsid: session.getSessionId(),
        txnNumber: NumberLong(2),
    }),
                         'failed to append docs B to bucket (retry write): ' + tojson(docsUpdateB));

    // This test case ensures that the batch size error handling is consistent with non-time-series
    // collections.
    assert.commandFailedWithCode(testDB.runCommand({
        insert: coll.getName(),
        documents: [],  // No documents
        lsid: session.getSessionId(),
        txnNumber: NumberLong(4),
    }),
                                 ErrorCodes.InvalidLength);

    const docs = docsInsert.concat(docsUpdateA, docsUpdateB);

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
    assert.eq(retriedCommandsCount += 3,
              transactionsServerStatus.retriedCommandsCount,
              'Incorrect statistic in db.serverStatus(): ' + tojson(transactionsServerStatus));
    assert.eq(retriedStatementsCount += docs.length,
              transactionsServerStatus.retriedStatementsCount,
              'Incorrect statistic in db.serverStatus(): ' + tojson(transactionsServerStatus));

    session.endSession();
};

const t = [
    ISODate("2021-01-20T00:00:00.000Z"),
    ISODate("2021-01-20T00:10:00.000Z"),
    ISODate("2021-01-20T00:20:00.000Z"),
    ISODate("2021-01-20T00:30:00.000Z"),
    ISODate("2021-01-20T00:40:00.000Z"),
    ISODate("2021-01-20T00:50:00.000Z"),
];

// One measurement per write operation.
runTest([{_id: 0, time: t[0], x: 0}], [{_id: 1, time: t[1], x: 1}], [{_id: 2, time: t[2], x: 2}]);
runTest([{_id: 0, time: t[0], x: 0}, {_id: 1, time: t[1], x: 1}],
        [{_id: 2, time: t[2], x: 2}, {_id: 3, time: t[3], x: 3}],
        [{_id: 4, time: t[4], x: 4}, {_id: 5, time: t[5], x: 5}]);

rst.stopSet();
})();
