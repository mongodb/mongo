/**
 * Tests that $_unpackBucket can still work properly if put after other stages like $match.
 */
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const measurementsPerBucket = 10;
const nBuckets = 10;
const conn = MongoRunner.runMongod({setParameter: {timeseriesBucketMaxCount: measurementsPerBucket}});

const testDB = conn.getDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const tsColl = testDB.getCollection("tsColl");
assert.commandWorked(testDB.createCollection(tsColl.getName(), {timeseries: {timeField: "start", metaField: "m"}}));

const bulk = tsColl.initializeUnorderedBulkOp();
const aMinuteInMs = 60 * 1000;
const seedDate = new Date("2020-11-30T12:10:05Z");
for (let i = 0; i < nBuckets; i++) {
    for (let j = 0; j < measurementsPerBucket; j++) {
        const seqNumber = i * measurementsPerBucket + j;
        bulk.insert({
            start: new Date(seedDate.valueOf() + seqNumber * aMinuteInMs),
            end: new Date(seedDate.valueOf() + (seqNumber + 1) * aMinuteInMs),
            m: "bucket_" + i,
            value: seqNumber,
        });
    }
}
assert.commandWorked(bulk.execute());
assert.eq(nBuckets, getTimeseriesCollForRawOps(testDB, tsColl).find().rawData().itcount());

// Use a filter to get some bucket IDs.
const bucketIds = getTimeseriesCollForRawOps(testDB, tsColl)
    .aggregate(
        [
            {
                $match: {
                    _id: {
                        $gt: ObjectId("5fc4e5c80000000000000000"),
                        $lt: ObjectId("5fc4ea7e0000000000000000"),
                    },
                    "control.max.end": {$lte: ISODate("2020-11-30T13:00:05Z")},
                },
            },
            {$sort: {_id: 1}},
        ],
        getRawOperationSpec(testDB),
    )
    .toArray();
// Should only get the IDs for 3 out of 10 buckets (bucket 2, 3 and 4).
assert.eq(3, bucketIds.length, bucketIds);
let ids = [];
for (let i = 0; i < bucketIds.length; i++) {
    ids.push(bucketIds[i]._id);
}
// Only unpack the designated buckets.
let getFilteredMeasurements = () =>
    getTimeseriesCollForRawOps(testDB, tsColl)
        .aggregate(
            [
                {$match: {_id: {$in: ids}}},
                {$_unpackBucket: {timeField: "start", metaField: "m"}},
                {$sort: {value: 1}},
                {$project: {_id: 0}},
            ],
            getRawOperationSpec(testDB),
        )
        .toArray();
let filteredMeasurements = getFilteredMeasurements();
let assertMeasurementsInBuckets = (lo, hi, measurements) => {
    let k = 0;
    // Only measurements from bucket 2, 3, and 4 are unpacked.
    for (let i = lo; i <= hi; i++) {
        for (let j = 0; j < measurementsPerBucket; j++, k++) {
            const seqNumber = i * measurementsPerBucket + j;
            assert.docEq(
                {
                    start: new Date(seedDate.valueOf() + seqNumber * aMinuteInMs),
                    end: new Date(seedDate.valueOf() + (seqNumber + 1) * aMinuteInMs),
                    m: "bucket_" + i,
                    value: seqNumber,
                },
                measurements[k],
            );
        }
    }
};
assertMeasurementsInBuckets(2, 4, filteredMeasurements);

// Delete bucket 2.
assert.commandWorked(getTimeseriesCollForRawOps(testDB, tsColl).deleteOne({_id: ids[0]}, getRawOperationSpec(testDB)));
filteredMeasurements = getFilteredMeasurements();
assertMeasurementsInBuckets(3, 4, filteredMeasurements);

MongoRunner.stopMongod(conn);
