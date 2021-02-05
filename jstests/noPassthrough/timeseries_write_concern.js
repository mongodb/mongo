/**
 * Tests that different write concerns are respected for time-series inserts, even if they are in
 * the same bucket.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/write_concern_util.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();

if (!TimeseriesTest.timeseriesCollectionsEnabled(primary)) {
    jsTestLog('Skipping test because the time-series collection feature flag is disabled');
    replTest.stopSet();
    return;
}

const dbName = jsTestName();
const testDB = primary.getDB(dbName);

const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';

coll.drop();
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

stopReplicationOnSecondaries(replTest);

const docs = [
    {_id: 0, [timeFieldName]: ISODate()},
    {_id: 1, [timeFieldName]: ISODate()},
];

const awaitInsert = startParallelShell(
    funWithArgs(function(dbName, collName, doc) {
        assert.commandWorked(db.getSiblingDB(dbName).runCommand(
            {insert: collName, documents: [doc], writeConcern: {w: 2}, comment: '{w: 2} insert'}));
    }, dbName, coll.getName(), docs[0]), primary.port);

// Wait for the {w: 2} insert to open a bucket.
assert.soon(() => {
    const serverStatus = assert.commandWorked(testDB.serverStatus());
    return serverStatus.hasOwnProperty('bucketCatalog') &&
        serverStatus.bucketCatalog.numOpenBuckets === 1;
});

// A {w: 1} insert should still be able to complete despite going into the same bucket as the {w: 2}
// insert, which is still outstanding.
assert.commandWorked(coll.insert(docs[1], {writeConcern: {w: 1}, ordered: false}));

// Ensure the {w: 2} insert has not yet completed.
assert.eq(
    assert.commandWorked(testDB.currentOp())
        .inprog
        .filter(op => op.ns === bucketsColl.getFullName() && op.command.comment === '{w: 2} insert')
        .length,
    1);

restartReplicationOnSecondaries(replTest);
awaitInsert();

assert.docEq(coll.find().toArray(), docs);
const buckets = bucketsColl.find().toArray();
assert.eq(buckets.length, 1, 'Expected one bucket but found: ' + tojson(buckets));
const serverStatus = assert.commandWorked(testDB.serverStatus()).bucketCatalog;
assert.eq(serverStatus.numOpenBuckets, 1, 'Expected one bucket but found: ' + tojson(serverStatus));

replTest.stopSet();
})();