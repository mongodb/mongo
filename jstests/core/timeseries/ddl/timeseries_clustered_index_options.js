/**
 * Tests that time-series buckets collections can be created with clusteredIndex options directly,
 * independent of the time-series collection creation command. This supports tools that clone
 * collections using the output of listCollections, which includes the clusteredIndex option.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   incompatible_with_preimages_by_default,
 * ]
 */
import {
    getTimeseriesCollForDDLOps,
    areViewlessTimeseriesEnabled,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const testDB = db.getSiblingDB(jsTestName());
const tsColl = testDB.clustered_index_options;

function testInvalidCreateBucketsCollectionOptions(invalidOptions, expectedErrorCodes) {
    assert.commandFailedWithCode(
        testDB.createCollection(getTimeseriesCollForDDLOps(testDB, tsColl).getName(), invalidOptions),
        expectedErrorCodes,
    );
}

// Tests time-series creation can be (somewhat)round-tripped to time-series buckets collection
// creation. Specifically, tests the 'listCollections' output on a time-series collection can be
// used to create a time-series buckets collection.
//
// Returns the options round-tripped from 'listCollections' to time-series buckets collection
// creation.
function roundTripTimeseriesToBucketsCreateOptions() {
    assert.commandWorked(testDB.dropDatabase());

    assert.commandWorked(
        testDB.createCollection(tsColl.getName(), {timeseries: {timeField: "time"}, expireAfterSeconds: 10}),
    );
    let res = assert.commandWorked(
        testDB.runCommand({listCollections: 1, filter: {name: getTimeseriesCollForDDLOps(testDB, tsColl).getName()}}),
    );
    const options = res.cursor.firstBatch[0].options;

    if (areViewlessTimeseriesEnabled(testDB)) {
        // Ensure clusteredIndex options is never returned on the listCollection logical format output
        assert(
            !options.hasOwnProperty("clusteredIndex"),
            `Found clusteredIndex property in logical collection metadata: ${options}`,
        );
    } else {
        // Ensure the 'clusteredIndex' legacy format is used {'clusteredIndex': true>}.
        assert.eq(tojson(options.clusteredIndex), tojson(true));
    }
    assert(tsColl.drop());

    assert.commandWorked(testDB.createCollection(getTimeseriesCollForDDLOps(testDB, tsColl).getName(), options));
    res = assert.commandWorked(
        testDB.runCommand({listCollections: 1, filter: {name: getTimeseriesCollForDDLOps(testDB, tsColl).getName()}}),
    );
    assert.eq(options, res.cursor.firstBatch[0].options);
    assert.commandWorked(testDB.dropDatabase());
    return options;
}

const options = roundTripTimeseriesToBucketsCreateOptions();

// Validate time-series buckets collection creation requires 'clusteredIndex' to be specified in the
// following format {'clusteredIndex': true}.
testInvalidCreateBucketsCollectionOptions({...options, clusteredIndex: {}}, ErrorCodes.IDLFailedToParse);
testInvalidCreateBucketsCollectionOptions({...options, clusteredIndex: "a"}, ErrorCodes.TypeMismatch);
testInvalidCreateBucketsCollectionOptions({...options, clusteredIndex: false}, ErrorCodes.InvalidOptions);

// Test the time-series buckets collection cannot be created with the generalized 'clusteredIndex'
// format used to create standard clustered collections.
// TODO SERVER-101614 remove custom error code from expected error list
testInvalidCreateBucketsCollectionOptions({...options, clusteredIndex: {key: {_id: 1}, unique: true}}, [
    5979703,
    ErrorCodes.InvalidOptions,
]);

// Validate additional 'idIndex' field fails to create the buckets collection when added to
// otherwise valid create options.
testInvalidCreateBucketsCollectionOptions(
    {...options, idIndex: {key: {_id: 1}, name: "_id_"}},
    ErrorCodes.InvalidOptions,
);

// Using the 'expireAfterSeconds' option on any namespace other than a time-series namespace or
// time-series buckets collection namespace should fail.
assert.commandFailedWithCode(testDB.createCollection("test", {expireAfterSeconds: 10}), ErrorCodes.InvalidOptions);
