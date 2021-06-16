/**
 * Tests that inserting into a time-series collection fails if the corresponding buckets collection
 * does not exist.
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load("jstests/libs/parallel_shell_helpers.js");

const conn = MongoRunner.runMongod();
const testDB = conn.getDB('test');

const timeFieldName = 'time';

let testCounter = 0;
const runTest = function(ordered, insertBeforeDrop, dropBucketsColl) {
    const coll = testDB[jsTestName() + '_' + testCounter++];

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    if (insertBeforeDrop) {
        assert.commandWorked(coll.insert({_id: 0, [timeFieldName]: ISODate()}));
    }

    const fp = configureFailPoint(conn, 'hangTimeseriesInsertBeforeWrite');

    const awaitDrop = startParallelShell(
        funWithArgs(
            function(collName, fpName, fpTimesEntered) {
                load("jstests/libs/fail_point_util.js");

                assert.commandWorked(db.adminCommand({
                    waitForFailPoint: fpName,
                    timesEntered: fpTimesEntered + 1,
                    maxTimeMS: kDefaultWaitForFailPointTimeout,
                }));

                assert(db[collName].drop());

                assert.commandWorked(db.adminCommand({configureFailPoint: fpName, mode: 'off'}));
            },
            dropBucketsColl ? 'system.buckets.' + coll.getName() : coll.getName(),
            fp.failPointName,
            fp.timesEntered),
        conn.port);

    assert.commandFailedWithCode(
        coll.insert({_id: 1, [timeFieldName]: ISODate()}, {ordered: ordered}),
        ErrorCodes.NamespaceNotFound);

    awaitDrop();
};

for (const dropBucketsColl of [false, true]) {
    runTest(false /* ordered */, false /* insertBeforeDrop */, dropBucketsColl);
    runTest(false /* ordered */, true /* insertBeforeDrop */, dropBucketsColl);
    runTest(true /* ordered */, false /* insertBeforeDrop */, dropBucketsColl);
    runTest(true /* ordered */, true /* insertBeforeDrop */, dropBucketsColl);
}

MongoRunner.stopMongod(conn);
})();
