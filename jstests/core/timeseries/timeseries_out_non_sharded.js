/**
 * Verifies that $out writes to a time-series collection from an unsharded collection.
 * There is a test for sharded source collections in jstests/sharding/timeseries_out_sharded.js.
 *
 * @tags: [
 *   references_foreign_collection,
 *   # TimeseriesAggTests doesn't handle stepdowns.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_71,
 *   # TODO SERVER-93841: should be removed once double-prefixing bug is investigated.
 *   not_allowed_with_signed_security_token,
 *   # TODO SERVER-88275: aggregation using internally a $mergeCursor stage can fail with
 *   # QueryPlanKilled in suites with random migrations because moveCollection change the collection
 *   # UUID by dropping and re-creating the collection. This specially happens on $out aggregations
 *   # where the collection doesn't live on the primary shard.
 *   assumes_balancer_off,
 *   # We cannot rename to a sharded collection, so the output collection must be unsharded.
 *   assumes_unsharded_collection,
 * ]
 */
import {TimeseriesAggTests} from "jstests/core/timeseries/libs/timeseries_agg_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const numHosts = 10;
const numIterations = 20;

const testDB = TimeseriesAggTests.getTestDb();
const dbName = testDB.getName();
assert.commandWorked(testDB.dropDatabase());

// Coll for the $out with timeseries.
const outColl = testDB.out_coll;
// Observer coll for the $out without timeseries.
const observerOutColl = testDB.observer_out_coll;

let [inColl, observerInColl] =
    TimeseriesAggTests.prepareInputCollections(numHosts, numIterations, true);

function runOutAndCompareResults({
    observer: observerPipeline,
    timeseries: timeseriesPipeline,
    options: expectedTSOptions = null,
    value: valueToCheck = null
}) {
    // Gets the expected results from a non time-series observer input collection.
    const observerResults = TimeseriesAggTests.getOutputAggregateResults(
        observerInColl, observerPipeline, null, false /* shouldDrop */);

    // Gets the actual results from a time-series input collection.
    const timeseriesResults =
        TimeseriesAggTests.getOutputAggregateResults(inColl, timeseriesPipeline, null, false);

    // Verifies that results are as expected in both the timeseries and observer cases.
    TimeseriesAggTests.verifyResults(timeseriesResults, observerResults);

    if (valueToCheck) {
        validateResultValues({result: timeseriesResults, value: valueToCheck});
    }

    // Make sure we only have 1 collection - either created if it didn't exist, or replaced the
    // existing one.
    const collections = testDB.getCollectionInfos({name: outColl.getName()});
    assert.eq(collections.length,
              1,
              `$out should replace the existing collection ${JSON.stringify(collections)}`);

    if (expectedTSOptions) {
        // Make sure the output collection is a timeseries collection.
        assert(collections[0]["options"]["timeseries"],
               `$out should maintain the timeseries collection ${JSON.stringify(collections)}`);

        const actualOptions = collections[0]["options"]["timeseries"];
        validateCollectionOptions({expected: expectedTSOptions, actual: actualOptions});

        // Make sure we have both the buckets collection and the view on the timeseries collection.
        const bucketsColl = assert.commandWorked(testDB.runCommand(
            {listCollections: 1, filter: {name: "system.buckets." + outColl.getName()}}));
        assert.eq(1, bucketsColl.cursor.firstBatch.length);

        assert.eq(1,
                  testDB.getCollection("system.views")
                      .find({viewOn: "system.buckets." + outColl.getName()})
                      .toArray()
                      .length);
    } else {
        // Make sure the output collection is not a timeseries collection.
        assert(
            !collections[0]["options"]["timeseries"],
            `$out should maintain the non-timeseries collection if no timeseries options are specified ${
                JSON.stringify(collections)}`);
    }
}

function validateResultValues({result: outResult, value: ExpectedValue}) {
    for (var i = 0; i < outResult.length; ++i) {
        // Make sure all the values for the fieldName specified are as expected.
        assert.eq(outResult[i],
                  {"time": ExpectedValue},
                  `expected value ${JSON.stringify(ExpectedValue)} but found ${
                      JSON.stringify(outResult[i])}`);
    }
}

function validateCollectionOptions({expected: expectedOptions, actual: actualOptions}) {
    for (let option in expectedOptions) {
        // Must loop through each option, since 'actualOptions' will contain default fields and
        // values that do not exist in 'expectedTSOptions'.
        assert.eq(expectedOptions[option],
                  actualOptions[option],
                  `expected options ${JSON.stringify(expectedOptions[option])} but found ${
                      JSON.stringify(actualOptions[option])}`);
    }
}

function dropOutCollections() {
    outColl.drop();
    observerOutColl.drop();
}

function createTimeseriesOutCollection() {
    const timeseriesOptions = {timeField: "time", metaField: "tags"};
    testDB.createCollection(outColl.getName(), {timeseries: timeseriesOptions});
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
    const timeseriesPipeline =
        [{$out: {db: dbName, coll: outColl.getName(), timeseries: tsOptions}}];

    runOutAndCompareResults(
        {observer: observerPipeline, timeseries: timeseriesPipeline, options: tsOptions});
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
    const timeseriesPipeline =
        [{$out: {db: dbName, coll: outColl.getName(), timeseries: expectedTSOptions}}];

    runOutAndCompareResults(
        {observer: observerPipeline, timeseries: timeseriesPipeline, options: expectedTSOptions});
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
        {$out: {db: testDB.getName(), coll: observerOutColl.getName()}}
    ];

    // Both inColl and outColl are timeseries collections. We want to make sure that a timeseries
    // collection can write to another timeseries collection without the timeseriesOptions, so we
    // don't specify those here.
    const timeseriesPipeline =
        [{$set: {"time": newDate}}, {$out: {db: testDB.getName(), coll: outColl.getName()}}];

    runOutAndCompareResults({
        observer: observerPipeline,
        timeseries: timeseriesPipeline,
        options: expectedTSOptions,
        value: newDate
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

    runOutAndCompareResults(
        {observer: observerPipeline, timeseries: timeseriesPipeline, options: expectedTSOptions});

    // Make sure the secondary index was maintained.
    const indexSpecs = testDB[outColl].getIndexes();
    assert.eq(indexSpecs.filter(index => index.name == "usage_guest_1").length, 1);
})();

(function testTimeseriesOutWithNonExistingDatabase() {
    // Drop both collections.
    dropOutCollections();

    // Test $out to time-series succeeds with a non-existent database.
    const destDB = testDB.getSiblingDB("outDifferentDB");
    assert.commandWorked(destDB.dropDatabase());

    const timeseriesPipeline =
        [{$out: {db: destDB.getName(), coll: outColl.getName(), timeseries: {timeField: "time"}}}];

    // TODO (SERVER-75856): Support implicit database creation for $merge and $out when running
    // aggregate on a mongos.
    try {
        inColl.aggregate(timeseriesPipeline);
        assert.eq(300, destDB[outColl.getName()].find().itcount());
    } catch (e) {
        assert.eq(e.code, ErrorCodes.NamespaceNotFound, e);
    }
})();

(function testCannotCreateTimeseriesCollFromNonTimeseriesColl() {
    // Drop both collections.
    dropOutCollections();

    // Insert document to ensure observerOutColl is a non-timeseries collection.
    observerOutColl.insert({a: 1});

    const pipeline = [{
        $out:
            {db: testDB.getName(), coll: observerOutColl.getName(), timeseries: {timeField: "time"}}
    }];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 7268700);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7268700);
})();

(function testCannotRunOutWithInvalidTimeseriesOptions() {
    // Drop both collections.
    dropOutCollections();

    const pipeline = [{
        $out: {
            db: testDB.getName(),
            coll: observerOutColl.getName(),
            timeseries: {timeField: "time", invalidField: "invalid"}
        }
    }];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 40415);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 40415);
})();

(function testCannotHaveMismatchingTimeField() {
    // Drop both collections.
    dropOutCollections();

    // Creates outColl as a TimeSeries collection with {timeField: "time", metaField: "tags"}.
    createTimeseriesOutCollection();

    // Timeseries options attempt to change the timeField, which is not allowed.
    const pipeline = [{
        $out:
            {db: testDB.getName(), coll: outColl.getName(), timeseries: {timeField: "invalidTime"}}
    }];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 7406103);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7406103);
})();

(function testCannotHaveMismatchingMetaField() {
    // Drop both collections.
    dropOutCollections();

    // Creates outColl as a TimeSeries collection with {timeField: "time", metaField: "tags"}.
    createTimeseriesOutCollection();

    // Timeseries options attempt to change the metaField, which is not allowed.
    const pipeline = [{
        $out: {
            db: testDB.getName(),
            coll: outColl.getName(),
            timeseries: {timeField: "time", metaField: "usage_guest_nice"}
        }
    }];

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
    const pipeline = [{
        $out: {
            db: testDB.getName(),
            coll: outColl.getName(),
            timeseries: {timeField: "time", bucketMaxSpanSeconds: 330, bucketRoundingSeconds: 330}
        }
    }];

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
    const pipeline = [{
        $out: {
            db: testDB.getName(),
            coll: outColl.getName(),
            timeseries: {timeField: "time", granularity: "minutes"}
        }
    }];

    assert.throwsWithCode(() => inColl.aggregate(pipeline), 7406103);
    assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7406103);
})();

(function testCannotHaveConflictingViews() {
    // Tests that an error is raised if a conflicting view exists.
    if (!FixtureHelpers.isMongos(testDB)) {  // can not shard a view.
        assert.commandWorked(testDB.createCollection("view_out", {viewOn: "out"}));
        const pipeline =
            [{$out: {db: testDB.getName(), coll: "view_out", timeseries: {timeField: "time"}}}];
        assert.throwsWithCode(() => inColl.aggregate(pipeline), 7268700);
        assert.throwsWithCode(() => observerInColl.aggregate(pipeline), 7268700);
    }
})();
