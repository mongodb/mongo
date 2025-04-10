/**
 * Tests the behavior of the $_internalBoundedSort stage with spilling to disk.
 * @tags: [
 *   requires_fcv_60,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 * ]
 */
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const kSmallMemoryLimit = 1024;
const conn = MongoRunner.runMongod(
    {setParameter: {internalQueryMaxBlockingSortMemoryUsageBytes: kSmallMemoryLimit}});

const dbName = jsTestName();
const testDB = conn.getDB(dbName);

const coll = testDB.timeseries_internal_bounded_sort;
coll.drop();
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: 't', metaField: 'm'}}));

// Insert some data.
{
    const numBatches = 10;
    const batchSize = 1000;
    const start = new Date();
    const intervalMillis = 1000;  // 1 second
    for (let i = 0; i < numBatches; ++i) {
        const batch = Array.from(
            {length: batchSize},
            (_, j) =>
                ({t: new Date(+start + i * batchSize * intervalMillis + j * intervalMillis)}));
        assert.commandWorked(coll.insert(batch));
        print(`Inserted ${i + 1} of ${numBatches} batches`);
    }
    assert.gt(getTimeseriesCollForRawOps(testDB, coll)
                  .aggregate([{$count: 'n'}], getRawOperationSpec(testDB))
                  .next()
                  .n,
              1,
              'Expected more than one bucket');
}

const unpackStage = getAggPlanStage(coll.explain().aggregate(), '$_internalUnpackBucket');

function assertSorted(result) {
    let prev = {t: -Infinity};
    for (const doc of result) {
        assert.lte(+prev.t, +doc.t, 'Found two docs not in time order: ' + tojson({prev, doc}));
        prev = doc;
    }
}

const aggOptions = Object.assign({allowDiskUse: false}, getRawOperationSpec(testDB));

// Test that memory limit would be hit by both implementations, and that both will error out if we
// don't enable disk use.
{
    assert.throwsWithCode(() => getTimeseriesCollForRawOps(testDB, coll)
                                    .aggregate(
                                        [
                                            unpackStage,
                                            {$_internalInhibitOptimization: {}},
                                            {$sort: {t: 1}},
                                        ],
                                        aggOptions),
                          ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

    assert.throwsWithCode(() => getTimeseriesCollForRawOps(testDB, coll)
                                    .aggregate(
                                        [
                                            {$sort: {'control.min.t': 1}},
                                            unpackStage,
                                            {
                                                $_internalBoundedSort: {
                                                    sortKey: {t: 1},
                                                    bound: {base: "min"},
                                                }
                                            },
                                        ],
                                        aggOptions),
                          ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
}

aggOptions.allowDiskUse = true;

// Test sorting the whole collection.
{
    const naive = getTimeseriesCollForRawOps(testDB, coll)
                      .aggregate(
                          [
                              unpackStage,
                              {$_internalInhibitOptimization: {}},
                              {$sort: {t: 1}},
                          ],
                          aggOptions)
                      .toArray();
    assertSorted(naive);

    const pipeline = [
        {$sort: {'control.min.t': 1}},
        unpackStage,
        {
            $_internalBoundedSort: {
                sortKey: {t: 1},
                bound: {base: "min"},
            }
        },
    ];
    const opt = getTimeseriesCollForRawOps(testDB, coll).aggregate(pipeline, aggOptions).toArray();
    assertSorted(opt);

    assert.eq(naive, opt);

    // Let's make sure the execution stats make sense.
    const stats = getAggPlanStage(getTimeseriesCollForRawOps(testDB, coll)
                                      .explain("executionStats")
                                      .aggregate(pipeline, aggOptions),
                                  '$_internalBoundedSort');
    assert.eq(stats.usedDisk, true);

    // We know each doc should have at least 8 bytes for time in both key and document.
    const docSize = stats.totalDataSizeSortedBytesEstimate / stats.nReturned;
    assert.gte(docSize, 16);
    const docsToTriggerSpill = Math.ceil(kSmallMemoryLimit / docSize);

    // We know we'll spill if we can't store all the docs from a single bucket within the memory
    // limit, so let's ensure that the total spills are at least what we'd expect if none of the
    // buckets overlap.
    const docsPerBucket =
        Math.floor(stats.nReturned /
                   getTimeseriesCollForRawOps(testDB, coll).count({}, getRawOperationSpec(testDB)));
    const spillsPerBucket = Math.floor(docsPerBucket / docsToTriggerSpill);
    assert.gt(spillsPerBucket, 0);
    assert.gt(stats.spilledDataStorageSize, 0);
    assert.gte(stats.spills,
               getTimeseriesCollForRawOps(testDB, coll).count({}, getRawOperationSpec(testDB)) *
                   spillsPerBucket);
}

// Test $sort + $limit.
{
    const naive = getTimeseriesCollForRawOps(testDB, coll)
                      .aggregate(
                          [
                              unpackStage,
                              {$_internalInhibitOptimization: {}},
                              {$sort: {t: 1}},
                              {$limit: 100},
                          ],
                          aggOptions)
                      .toArray();
    assertSorted(naive);
    assert.eq(100, naive.length);

    const opt =
        getTimeseriesCollForRawOps(testDB, coll)
            .aggregate(
                [
                    {$sort: {'control.min.t': 1}},
                    unpackStage,
                    {$_internalBoundedSort: {sortKey: {t: 1}, bound: {base: "min"}, limit: 100}}
                ],
                aggOptions)
            .toArray();
    assertSorted(opt);
    assert.eq(100, opt.length);

    assert.eq(naive, opt);
}

MongoRunner.stopMongod(conn);