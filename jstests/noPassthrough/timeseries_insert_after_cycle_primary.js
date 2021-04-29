/**
 * Tests that time-series inserts are properly handled when a node steps down from primary and then
 * later steps back up.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

if (!TimeseriesTest.timeseriesCollectionsEnabled(replTest.getPrimary())) {
    jsTestLog('Skipping test because the time-series collection feature flag is disabled');
    replTest.stopSet();
    return;
}

const dbName = 'test';
const numColls = 3;

const testDB = function() {
    return replTest.getPrimary().getDB(dbName);
};

const coll = function(num) {
    return testDB()[jsTestName() + '_' + num];
};

const bucketsColl = function(num) {
    return testDB()['system.buckets.' + coll(num).getName()];
};

const timeFieldName = 'time';
const metaFieldName = 'meta';

const createColl = function(num) {
    assert.commandWorked(testDB().createCollection(
        coll(num).getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
};

for (let i = 0; i < numColls; i++) {
    createColl(i);
}

const docs = [
    {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: 0},
    {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: 0},
];

for (let i = 0; i < numColls; i++) {
    assert.commandWorked(coll(i).insert(docs[0]));
}

replTest.stepUp(replTest.getSecondary());

// Manually update the bucket for collection 1.
assert.commandWorked(bucketsColl(1).update({}, {$set: {meta: 1}}));
assert.commandWorked(bucketsColl(1).update({}, {$set: {meta: 0}}));

// Drop, recreate, and reinsert the bucket for collection 2.
assert(coll(2).drop());
createColl(2);
assert.commandWorked(coll(2).insert(docs[0]));

// Step back up the original primary.
replTest.stepUp(replTest.getSecondary());

for (let i = 0; i < numColls; i++) {
    assert.commandWorked(coll(i).insert(docs[1]));
}

const checkColl = function(num, numBuckets) {
    jsTestLog('Checking collection ' + num);
    assert.docEq(coll(num).find().sort({_id: 1}).toArray(), docs);
    const buckets = bucketsColl(num).find().toArray();
    assert.eq(buckets.length,
              numBuckets,
              'Expected ' + numBuckets + ' bucket(s) but found: ' + tojson(buckets));
};

// For collection 0, the original bucket should still be usable.
checkColl(0, 1);
// For collections 1 and 2, the original bucket should have been closed.
checkColl(1, 2);
checkColl(2, 2);

replTest.stopSet();
})();
