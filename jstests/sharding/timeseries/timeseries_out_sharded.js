/**
 * Verifies that $out can write to time-series collections from sharded collections.
 *
 *
 * @tags: [
 *   # TimeseriesAggTests doesn't handle stepdowns.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {TimeseriesAggTests} from "jstests/core/timeseries/libs/timeseries_agg_helpers.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
    getTimeseriesCollForDDLOps,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const numHosts = 10;
const splitPoint = numHosts / 2;
const numIterations = 20;

const st = new ShardingTest({shards: 2});
const dbName = jsTestName();
const testDB = st.s.getDB(dbName);
assert.commandWorked(testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

let [inColl, observerInColl] = TimeseriesAggTests.prepareInputCollections(numHosts, numIterations, true, testDB);

// Split the data over 2 shards. The test data has tags uniformly distributed from 1-10, so for half
// the data to be on each shard, we will split at tags = 5.
testDB.adminCommand({shardCollection: inColl.getFullName(), key: {"tags": 1}});
assert.commandWorked(
    st.s.adminCommand({
        split: getTimeseriesCollForDDLOps(testDB, inColl).getFullName(),
        middle: {"meta": splitPoint},
    }),
);
assert.commandWorked(
    st.s.adminCommand({
        movechunk: getTimeseriesCollForDDLOps(testDB, inColl).getFullName(),
        find: {"meta": splitPoint},
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);
observerInColl.createIndex({tags: 1});
st.shardColl(observerInColl.getName(), {"tags": 1}, {"tags": splitPoint}, true, dbName);

// Coll for the $out with timeseries.
const outColl = testDB.out_coll;
// Observer coll for the $out without timeseries.
const observerOutColl = testDB.observer_out_coll;

function runOutAndCompareResults({
    observer: observerPipeline,
    timeseries: timeseriesPipeline,
    options: expectedTSOptions = null,
    value: valueToCheck = null,
    outputDB: outputDB = testDB,
    checkForDefaultIndex: checkForDefaultIndex = true,
}) {
    // Gets the expected results from a non time-series observer input collection.
    const observerResults = TimeseriesAggTests.getOutputAggregateResults(
        observerInColl,
        observerPipeline,
        null,
        false,
        outputDB,
    );

    // Gets the actual results from a time-series input collection.
    const timeseriesResults = TimeseriesAggTests.getOutputAggregateResults(
        inColl,
        timeseriesPipeline,
        null,
        false,
        outputDB,
    );

    // Verifies that results are as expected in both the timeseries and observer cases.
    TimeseriesAggTests.verifyResults(timeseriesResults, observerResults);

    if (valueToCheck) {
        validateResultValues({result: timeseriesResults, value: valueToCheck});
    }

    // Make sure we only have 1 collection - either created if it didn't exist, or replaced the
    // existing one.
    const collections = outputDB.getCollectionInfos({name: outColl.getName()});
    assert.eq(collections.length, 1, `$out should replace the existing collection ${JSON.stringify(collections)}`);

    if (expectedTSOptions) {
        // Make sure the output collection is a timeseries collection.
        assert(
            collections[0]["options"]["timeseries"],
            `$out should maintain the timeseries collection ${JSON.stringify(collections)}`,
        );

        const actualOptions = collections[0]["options"]["timeseries"];
        validateCollectionOptions({expected: expectedTSOptions, actual: actualOptions});

        // TODO SERVER-101784 remove these checks once only viewless timeseries exist.
        const bucketsCollCount = assert.commandWorked(
            outputDB.runCommand({
                listCollections: 1,
                filter: {name: getTimeseriesBucketsColl(outColl.getName())},
            }),
        ).cursor.firstBatch.length;
        const viewCount = outputDB
            .getCollection("system.views")
            .find({viewOn: getTimeseriesBucketsColl(outColl.getName())})
            .toArray().length;

        if (checkForDefaultIndex) {
            let containsDefaultIndex = false;
            for (const index of outColl.getIndexes()) {
                if (
                    index == timeseriesDefaultIndex() ||
                    bsonUnorderedFieldsCompare(index, timeseriesDefaultIndex()) == 0
                ) {
                    containsDefaultIndex = true;
                    break;
                }
            }

            assert(
                containsDefaultIndex,
                "Output collection does not contain default timeseries index: " + tojson(timeseriesDefaultIndex()),
            );
        }

        if (areViewlessTimeseriesEnabled(testDB)) {
            // Make sure that neither the buckets collection, nor the view get created.
            assert.eq(0, bucketsCollCount);
            assert.eq(0, viewCount);
        } else {
            // Make sure we have both the buckets collection and the view.
            assert.eq(1, bucketsCollCount);
            assert.eq(1, viewCount);
        }
    } else {
        // Make sure the output collection is not a timeseries collection.
        assert(
            !collections[0]["options"]["timeseries"],
            `$out should maintain the non-timeseries collection if no timeseries options are specified ${JSON.stringify(
                collections,
            )}`,
        );
    }
}

function validateResultValues({result: outResult, value: ExpectedValue}) {
    for (let i = 0; i < outResult.length; ++i) {
        // Make sure all the values for the fieldName specified are as expected.
        assert.eq(
            outResult[i],
            {"time": ExpectedValue},
            `expected value ${JSON.stringify(ExpectedValue)} but found ${JSON.stringify(outResult[i])}`,
        );
    }
}

function validateCollectionOptions({expected: expectedOptions, actual: actualOptions}) {
    for (let option in expectedOptions) {
        // Must loop through each option, since 'actualOptions' will contain default fields and
        // values that do not exist in 'expectedTSOptions'.
        assert.eq(
            expectedOptions[option],
            actualOptions[option],
            `expected options ${JSON.stringify(expectedOptions[option])} but found ${JSON.stringify(
                actualOptions[option],
            )}`,
        );
    }
}

function dropOutCollections() {
    outColl.drop();
    observerOutColl.drop();
}

function timeseriesOptions() {
    return {timeField: "time", metaField: "tags"};
}

function createTimeseriesOutCollection() {
    testDB.createCollection(outColl.getName(), {timeseries: timeseriesOptions()});
}

function timeseriesDefaultIndex() {
    const metaField = timeseriesOptions()["metaField"];
    const timeField = timeseriesOptions()["timeField"];

    return {
        "v": 2,
        "key": {
            [metaField]: 1,
            [timeField]: 1,
        },
        "name": metaField + "_1_" + timeField + "_1",
    };
}

(function testSourceTimeseriesOutToNonTimeseriesCollection() {
    // Drop both collections.
    dropOutCollections();

    // Add an arbitrary document to create the non-timeseries collections.
    outColl.insert({a: 1});
    observerOutColl.insert({a: 1});

    const observerPipeline = [{$out: observerOutColl.getName()}];
    const timeseriesPipeline = [{$out: outColl.getName()}];

    runOutAndCompareResults({observer: observerPipeline, timeseries: timeseriesPipeline});
})();

(function testSourceTimeseriesOutToNonExistingCollection() {
    // Drop both collections.
    dropOutCollections();

    const observerPipeline = [{$out: observerOutColl.getName()}];

    const tsOptions = {timeField: "time", metaField: "tags"};
    // Having the timeseries option should cause the result $out collection to be a timeseries
    // collection.
    const timeseriesPipeline = [{$out: {db: dbName, coll: outColl.getName(), timeseries: tsOptions}}];

    runOutAndCompareResults({observer: observerPipeline, timeseries: timeseriesPipeline, options: tsOptions});
})();

(function testSourceTimeseriesOutToTimeseriesCollection() {
    // Drop both collections.
    dropOutCollections();

    // Create the timeseries out collection.
    createTimeseriesOutCollection();

    // Change an option in the existing time-series collections, so that we can check it stays the
    // same after running $out.
    assert.commandWorked(testDB.runCommand({collMod: outColl.getName(), expireAfterSeconds: 3600}));

    const collections = testDB.getCollectionInfos({name: outColl.getName()});
    assert.eq(collections.length, 1, collections);

    // Get the original timeseries options, these should stay the same post $out.
    const expectedTSOptions = collections[0]["options"]["timeseries"];

    const observerPipeline = [{$out: {db: dbName, coll: observerOutColl.getName()}}];
    const timeseriesPipeline = [{$out: {db: dbName, coll: outColl.getName(), timeseries: expectedTSOptions}}];

    runOutAndCompareResults({observer: observerPipeline, timeseries: timeseriesPipeline, options: expectedTSOptions});
})();

(function testTimeseriesOutToTimeseriesCollectionWithoutOptions() {
    // Drop both collections.
    dropOutCollections();

    // Create the timeseries out collection.
    createTimeseriesOutCollection();

    // Change an option in the existing time-series collections, so that we can check it stays the
    // same after running $out.
    assert.commandWorked(testDB.runCommand({collMod: outColl.getName(), expireAfterSeconds: 3600}));

    const collections = testDB.getCollectionInfos({name: outColl.getName()});
    assert.eq(collections.length, 1, collections);

    // Get the original timeseries options, these should stay the same post $out.
    const expectedTSOptions = collections[0]["options"]["timeseries"];

    // Change the "time" field in the pipeline, so we can confirm the value is changed in the
    // result.
    const newDate = new Date();
    const observerPipeline = [
        {$set: {"time": newDate}},
        {$out: {db: testDB.getName(), coll: observerOutColl.getName()}},
    ];

    // Both inColl and outColl are timeseries collections. We want to make sure that a timeseries
    // collection can write to another timeseries collection without the timeseriesOptions, so we
    // don't specify those here.
    const timeseriesPipeline = [{$set: {"time": newDate}}, {$out: {db: testDB.getName(), coll: outColl.getName()}}];

    runOutAndCompareResults({
        observer: observerPipeline,
        timeseries: timeseriesPipeline,
        options: expectedTSOptions,
        value: newDate,
    });
})();

(function testTimeseriesOutPreservesIndexes() {
    // Drop both collections.
    dropOutCollections();

    // Create the timeseries out collection.
    createTimeseriesOutCollection();

    // Add a secondary index.
    assert.commandWorked(testDB[outColl].createIndex({usage_guest: 1}));

    const collections = testDB.getCollectionInfos({name: outColl.getName()});
    assert.eq(collections.length, 1, collections);

    const expectedTSOptions = collections[0]["options"]["timeseries"];

    const observerPipeline = [{$out: {db: testDB.getName(), coll: observerOutColl.getName()}}];
    const timeseriesPipeline = [{$out: {db: testDB.getName(), coll: outColl.getName()}}];

    runOutAndCompareResults({observer: observerPipeline, timeseries: timeseriesPipeline, options: expectedTSOptions});

    // Make sure the secondary index was maintained.
    const indexSpecs = testDB[outColl].getIndexes();
    assert.eq(indexSpecs.filter((index) => index.name == "usage_guest_1").length, 1);
})();

(function testTimeseriesOutWithNonExistingDatabaseTimeseriesOutput() {
    // Drop both collections.
    dropOutCollections();

    // Test $out to time-series succeeds with a non-existent database.
    const destDB = testDB.getSiblingDB("outDifferentDB");
    assert.commandWorked(destDB.dropDatabase());

    const tsOptions = {timeField: "time"};

    const timeseriesPipeline = [{$out: {db: destDB.getName(), coll: outColl.getName(), timeseries: tsOptions}}];

    const observerPipeline = [{$out: {db: destDB.getName(), coll: observerOutColl.getName()}}];

    // TODO SERVER-111869: Test should pass with checkForDefaultIndex set to true once bug is fixed.
    runOutAndCompareResults({
        observer: observerPipeline,
        timeseries: timeseriesPipeline,
        options: tsOptions,
        outputDB: destDB,
        checkForDefaultIndex: false,
    });
})();

(function testTimeseriesOutWithNonExistingDatabaseNonTimeseriesOutput() {
    // Drop both collections.
    dropOutCollections();

    // Test $out to time-series succeeds with a non-existent database.
    const destDB = testDB.getSiblingDB("outDifferentDB");
    assert.commandWorked(destDB.dropDatabase());

    const timeseriesPipeline = [{$out: {db: destDB.getName(), coll: outColl.getName()}}];

    const observerPipeline = [{$out: {db: destDB.getName(), coll: observerOutColl.getName()}}];

    runOutAndCompareResults({observer: observerPipeline, timeseries: timeseriesPipeline, outputDB: destDB});
})();

(function testCannotCreateTimeseriesCollFromNonTimeseriesColl() {
    // Drop both collections.
    dropOutCollections();

    // Insert document to ensure observerOutColl is a non-timeseries collection.
    observerOutColl.insert({a: 1});

    const pipeline = [
        {
            $out: {db: testDB.getName(), coll: observerOutColl.getName(), timeseries: {timeField: "time"}},
        },
    ];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 7268700);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7268700);
})();

(function testCannotRunOutWithInvalidTimeseriesOptions() {
    // Drop both collections.
    dropOutCollections();

    const pipeline = [
        {
            $out: {
                db: testDB.getName(),
                coll: observerOutColl.getName(),
                timeseries: {timeField: "time", invalidField: "invalid"},
            },
        },
    ];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 40415);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 40415);
})();

(function testCannotHaveMismatchingTimeField() {
    // Drop both collections.
    dropOutCollections();

    // Creates outColl as a TimeSeries collection with {timeField: "time", metaField: "tags"}.
    createTimeseriesOutCollection();

    // Timeseries options attempt to change the timeField, which is not allowed.
    const pipeline = [
        {
            $out: {db: testDB.getName(), coll: outColl.getName(), timeseries: {timeField: "invalidTime"}},
        },
    ];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 7406103);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7406103);
})();

(function testCannotHaveMismatchingMetaField() {
    // Drop both collections.
    dropOutCollections();

    // Creates outColl as a TimeSeries collection with {timeField: "time", metaField: "tags"}.
    createTimeseriesOutCollection();

    // Timeseries options attempt to change the metaField, which is not allowed.
    const pipeline = [
        {
            $out: {
                db: testDB.getName(),
                coll: outColl.getName(),
                timeseries: {timeField: "time", metaField: "usage_guest_nice"},
            },
        },
    ];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 7406103);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7406103);
})();

(function testCannotHaveMismatchingBucketManSpanSeconds() {
    // Drop both collections.
    dropOutCollections();

    // Creates outColl as a TimeSeries collection with {timeField: "time", metaField: "tags"}, the
    // rest of the options are the default.
    createTimeseriesOutCollection();

    // Timeseries options attempt to change the bucketManSpanSeconds, which is not allowed.
    const pipeline = [
        {
            $out: {
                db: testDB.getName(),
                coll: outColl.getName(),
                timeseries: {timeField: "time", bucketMaxSpanSeconds: 330, bucketRoundingSeconds: 330},
            },
        },
    ];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 7406103);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7406103);
})();

(function testCannotHaveMismatchingGranularity() {
    // Drop both collections.
    dropOutCollections();

    // Creates outColl as a TimeSeries collection with {timeField: "time", metaField: "tags"}, the
    // rest of the options are the default.
    createTimeseriesOutCollection();

    // Timeseries options attempt to change the granularity, which is not allowed.
    const pipeline = [
        {
            $out: {
                db: testDB.getName(),
                coll: outColl.getName(),
                timeseries: {timeField: "time", granularity: "minutes"},
            },
        },
    ];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 7406103);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7406103);
})();

// TODO SERVER-111600: Remove this test once only viewless timeseries exists
(function testCannotHaveConflictingViews() {
    // Tests that an error is raised if a conflicting view exists.
    assert.commandWorked(testDB.createCollection("view_out", {viewOn: "out"}));
    const pipeline = [{$out: {db: testDB.getName(), coll: "view_out", timeseries: {timeField: "time"}}}];
    assert.throwsWithCode(() => inColl.aggregate(pipeline), 7268700);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7268700);
})();

(function testCannotRunWithRawData() {
    // Drop both collections.
    dropOutCollections();

    // Creates outColl as a TimeSeries collection with {timeField: "time", metaField: "tags"}, the
    // rest of the options are the default.
    createTimeseriesOutCollection();

    let outCollName = outColl.getName();
    let rawDataSpec = {rawData: true};

    if (!areViewlessTimeseriesEnabled(testDB)) {
        outCollName = "system.buckets." + outCollName;
        rawDataSpec = {};
    }

    const pipeline = [
        {
            $out: {
                db: testDB.getName(),
                coll: outCollName,
                timeseries: timeseriesOptions(),
            },
        },
    ];

    // TODO SERVER-108560: Remove 17835 error code
    assert.throwsWithCode(() => inColl.aggregate(pipeline, rawDataSpec), [17385, 10203900]);
})();

st.stop();
