/**
 * Tests that $_unpackBucket can still work properly if put after other stages like $match.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const measurementsPerBucket = 10;
const nBuckets = 10;
const conn =
    MongoRunner.runMongod({setParameter: {timeseriesBucketMaxCount: measurementsPerBucket}});
const testDB = conn.getDB(jsTestName());

if (!TimeseriesTest.timeseriesCollectionsEnabled(testDB.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

assert.commandWorked(testDB.dropDatabase());
const tsColl = testDB.getCollection("tsColl");
assert.commandWorked(testDB.createCollection(
    tsColl.getName(), {timeseries: {timeField: "start", metaField: "meta"}}));
const sysColl = testDB.getCollection("system.buckets." + tsColl.getName());

const bulk = tsColl.initializeUnorderedBulkOp();
const aMinuteInMs = 60 * 1000;
const seedDate = new Date("2020-11-30T12:10:05Z");
for (let i = 0; i < nBuckets; i++) {
    for (let j = 0; j < measurementsPerBucket; j++) {
        const seqNumber = i * measurementsPerBucket + j;
        bulk.insert({
            start: new Date(seedDate.valueOf() + seqNumber * aMinuteInMs),
            end: new Date(seedDate.valueOf() + (seqNumber + 1) * aMinuteInMs),
            meta: "bucket_" + i,
            value: seqNumber,
        });
    }
}
assert.commandWorked(bulk.execute());
assert.eq(nBuckets, sysColl.find().itcount());

// Use a filter to get some bucket IDs.
const bucketIds = sysColl
                      .aggregate([
                          {
                              $match: {
                                  _id: {
                                      $gt: ObjectId("5fc4e5c80000000000000000"),
                                      $lt: ObjectId("5fc4ea7e0000000000000000")
                                  },
                                  "control.max.end": {$lte: ISODate("2020-11-30T13:00:05Z")}
                              }
                          },
                          {$sort: {_id: 1}}
                      ])
                      .toArray();
// Should only get the IDs for 3 out of 10 buckets (bucket 2, 3 and 4).
assert.eq(3, bucketIds.length, bucketIds);
let ids = [];
for (let i = 0; i < bucketIds.length; i++) {
    ids.push(bucketIds[i]._id);
}
// Only unpack the designated buckets.
let getFilteredMeasurements = () =>
    sysColl
        .aggregate([
            {$match: {_id: {$in: ids}}},
            {$_unpackBucket: {timeField: "start", metaField: "meta"}},
            {$sort: {value: 1}},
            {$project: {_id: 0}}
        ])
        .toArray();
let filteredMeasurements = getFilteredMeasurements();
let assertMeasurementsInBuckets = (lo, hi, measurements) => {
    let k = 0;
    // Only measurements from bucket 2, 3, and 4 are unpacked.
    for (let i = lo; i <= hi; i++) {
        for (let j = 0; j < measurementsPerBucket; j++, k++) {
            const seqNumber = i * measurementsPerBucket + j;
            assert.docEq({
                start: new Date(seedDate.valueOf() + seqNumber * aMinuteInMs),
                end: new Date(seedDate.valueOf() + (seqNumber + 1) * aMinuteInMs),
                meta: "bucket_" + i,
                value: seqNumber,
            },
                         measurements[k]);
        }
    }
};
assertMeasurementsInBuckets(2, 4, filteredMeasurements);

// Delete bucket 2.
assert.commandWorked(sysColl.deleteOne({_id: ids[0]}));
filteredMeasurements = getFilteredMeasurements();
assertMeasurementsInBuckets(3, 4, filteredMeasurements);

MongoRunner.stopMongod(conn);
})();
