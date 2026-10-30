/**
 * A property-based test that stresses the slicing of parallel collection validation. Collections
 * of varying size, density and RecordId format (regular and time-series) are validated with
 * varying slice options, and the outcome must match that of a validation that does not slice at
 * all and must not report any out-of-order records.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */

import {describe, it} from "jstests/libs/mochalite.js";
import {kPbtInterruptAfterTimeLimitMs} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

const kNumRuns = 100;
const kSeed = 42;

const kDefaultTargetRecordsPerSlice = 50000;
const kDefaultMaxSlices = 256;

let conn;
let db;
let coll;

function setSlicing(targetRecordsPerSlice, maxSlices) {
    assert.commandWorked(
        conn.adminCommand({
            setParameter: 1,
            validateParallelTargetRecordsPerSlice: targetRecordsPerSlice,
            validateParallelMaxRecordStoreSlices: maxSlices,
        }),
    );
}

function validateWith(options, targetRecordsPerSlice, maxSlices) {
    setSlicing(targetRecordsPerSlice, maxSlices);
    return assert.commandWorked(coll.validate(options));
}

// The validation output that must not depend on how the record store was sliced. The slice count
// is how a sliced validation is told apart, so it is left out. It is also absent when the
// validation degrades to a single slice.
function outcome(res) {
    const {nParallelSlices, ...rest} = res;
    return rest;
}

// The number of records validation traverses, which for a time-series collection is its buckets.
function countRecords(timeseries) {
    if (!timeseries) {
        return coll.countDocuments({});
    }
    const [stats] = coll.aggregate([{$collStats: {storageStats: {}}}]).toArray();
    return stats.storageStats.timeseries.bucketCount;
}

function createAndPopulate({timeseries, numDocs, deleteEvery, holes, numMetas}) {
    coll.drop();
    // A time-series collection's buckets are clustered on an ObjectId, so its RecordIds are not
    // integers and slicing it takes the random cursor path rather than the arithmetic one.
    const options = timeseries ? {timeseries: {timeField: "t", metaField: "m"}} : {};
    assert.commandWorked(db.createCollection(coll.getName(), options));

    // Each distinct meta value gets buckets of its own, so this sets how many records there are.
    // The _id of a regular document is its position, which makes ranges of them easy to delete.
    const docOf = (i) =>
        timeseries
            ? {t: new Date(1700000000000 + i * 1000), m: i % numMetas, a: i % 7}
            : {_id: i, a: i % 7, padding: "x".repeat(i % 50)};
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        bulk.insert(docOf(i));
    }
    assert.commandWorked(bulk.execute());

    // Integer RecordIds are allocated consecutively whatever the _id is, and never reused, so the
    // only way to widely space the records is to delete runs of them. The gaps put slice
    // boundaries where no record exists. A time-series collection can only be deleted from by its
    // meta field.
    const key = timeseries ? "m" : "_id";
    const keyCount = timeseries ? numMetas : numDocs;
    for (const {start, length} of holes) {
        const lo = Math.floor(start * keyCount);
        const hi = Math.min(keyCount, lo + Math.ceil(length * keyCount));
        assert.commandWorked(coll.deleteMany({[key]: {$gte: lo, $lt: hi}}));
    }
    // Scattered deletes leave small gaps throughout. Zero keeps every remaining record.
    if (deleteEvery > 0) {
        assert.commandWorked(coll.deleteMany({[key]: {$mod: [deleteEvery, 0]}}));
    }
}

const workloadArb = fc.record({
    timeseries: fc.boolean(),
    numDocs: fc.integer({min: 300, max: 25_000}),
    deleteEvery: fc.oneof(fc.constant(0), fc.integer({min: 2, max: 9})),
    // Runs of consecutive records to delete, as fractions of the collection, which leave wide gaps
    // in the RecordIds.
    holes: fc.array(
        fc.record({
            start: fc.double({min: 0, max: 1, noNaN: true, maxExcluded: true}),
            length: fc.double({min: 0, max: 0.7, noNaN: true}),
        }),
        {maxLength: 4},
    ),
    // Distinct meta values of a time-series collection, which is how many buckets it has.
    numMetas: fc.integer({min: 1, max: 5000}),
});

const slicingArb = fc.record({
    // Zero disables slicing; small values produce many slices.
    targetRecordsPerSlice: fc.integer({min: 0, max: 50_000}),
    maxSlices: fc.integer({min: 1, max: 1000}),
    validateOptions: fc.constantFrom(
        {full: true},
        {full: true, checkBSONConformance: true},
        {full: false},
    ),
});

conn = MongoRunner.runMongod({
    setParameter: {featureFlagParallelCollectionValidation: true},
});
db = conn.getDB("validate_parallel_slicing_pbt");
coll = db.coll;

describe("Record Store Slicing PBT", () => {
    it("validates all records regardless of slicing options", () => {
        let numSlicedRuns = 0;
        try {
            fc.assert(
                fc
                    .property(workloadArb, slicingArb, (workload, slicing) => {
                        createAndPopulate(workload);
                        const expectedCount = countRecords(workload.timeseries);

                        const unsliced = validateWith(slicing.validateOptions, 0, 1);
                        const sliced = validateWith(
                            slicing.validateOptions,
                            slicing.targetRecordsPerSlice,
                            slicing.maxSlices,
                        );

                        assert(
                            !("nParallelSlices" in unsliced),
                            "unsliced validation reported slices",
                            {
                                unsliced,
                            },
                        );
                        if ("nParallelSlices" in sliced) {
                            ++numSlicedRuns;
                            assert.gte(
                                sliced.nParallelSlices,
                                2,
                                "a single slice is not reported",
                                {
                                    sliced,
                                },
                            );
                            assert.lte(
                                sliced.nParallelSlices,
                                slicing.maxSlices,
                                "too many slices",
                                {
                                    sliced,
                                    slicing,
                                },
                            );
                        }

                        if (!sliced.valid || sliced.nrecords !== expectedCount) {
                            jsTest.log.error("Sliced validation disagrees with the collection", {
                                workload,
                                slicing,
                                expectedCount,
                                sliced,
                            });
                            return false;
                        }
                        if (bsonWoCompare(outcome(unsliced), outcome(sliced)) !== 0) {
                            jsTest.log.error("Sliced validation differs from unsliced validation", {
                                workload,
                                slicing,
                                unsliced: outcome(unsliced),
                                sliced: outcome(sliced),
                            });
                            return false;
                        }
                        return true;
                    })
                    .beforeEach(() => setSlicing(kDefaultTargetRecordsPerSlice, kDefaultMaxSlices))
                    .afterEach(() => coll.drop()),
                {
                    numRuns: kNumRuns,
                    seed: kSeed,
                    interruptAfterTimeLimit: kPbtInterruptAfterTimeLimitMs,
                },
            );
            jsTest.log.info("Runs validated with slicing", {numSlicedRuns, kNumRuns});
        } finally {
            setSlicing(kDefaultTargetRecordsPerSlice, kDefaultMaxSlices);
            MongoRunner.stopMongod(conn);
        }
    });
});
