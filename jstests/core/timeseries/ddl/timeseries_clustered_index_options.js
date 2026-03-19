/**
 * Tests that viewless time-series collections can be created with clusteredIndex options directly,
 * independent of the time-series collection creation command. This supports tools that clone
 * collections using the output of listCollections, which includes the clusteredIndex option.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   incompatible_with_preimages_by_default,
 *   requires_timeseries,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

const testDB = db.getSiblingDB(jsTestName());
const tsColl = testDB.clustered_index_options;

function testInvalidCreateCollectionOptions(invalidOptions, expectedErrorCodes) {
    const collToClone = tsColl;
    assert.commandFailedWithCode(testDB.createCollection(collToClone.getName(), invalidOptions), expectedErrorCodes);
}

// Tests time-series creation can be (somewhat)round-tripped to time-series collection
// creation. Specifically, tests the 'listCollections' output on a viewless time-series collection
// can be used to re-create the time-series collection.
//
// Returns the options round-tripped from 'listCollections' to time-series collection creation.
function roundTripTimeseriesCreateOptions() {
    assert.commandWorked(testDB.dropDatabase());

    assert.commandWorked(
        testDB.createCollection(tsColl.getName(), {timeseries: {timeField: "time"}, expireAfterSeconds: 10}),
    );
    const collToClone = tsColl;
    let res = assert.commandWorked(testDB.runCommand({listCollections: 1, filter: {name: collToClone.getName()}}));
    const options = res.cursor.firstBatch[0].options;

    // Ensure clusteredIndex options is never returned on the listCollection logical format output.
    assert(
        !options.hasOwnProperty("clusteredIndex"),
        `Found clusteredIndex property in logical collection metadata: ${options}`,
    );

    // With rawData: true, listCollections exposes the raw physical format of the collection,
    // which includes clusteredIndex: true.
    res = assert.commandWorked(
        testDB.runCommand({listCollections: 1, rawData: true, filter: {name: collToClone.getName()}}),
    );
    assert.eq(tojson(res.cursor.firstBatch[0].options.clusteredIndex), tojson(true));

    assert(tsColl.drop());

    assert.commandWorked(testDB.createCollection(collToClone.getName(), options));
    res = assert.commandWorked(testDB.runCommand({listCollections: 1, filter: {name: collToClone.getName()}}));
    assert.eq(options, res.cursor.firstBatch[0].options);
    assert.commandWorked(testDB.dropDatabase());
    return options;
}

const options = roundTripTimeseriesCreateOptions();

// Validate time-series collection creation requires 'clusteredIndex' to be specified in the
// following format {'clusteredIndex': true}.
testInvalidCreateCollectionOptions({...options, clusteredIndex: {}}, ErrorCodes.IDLFailedToParse);
testInvalidCreateCollectionOptions({...options, clusteredIndex: "a"}, ErrorCodes.TypeMismatch);
testInvalidCreateCollectionOptions({...options, clusteredIndex: false}, ErrorCodes.InvalidOptions);

// Test the time-series collection cannot be created with the generalized 'clusteredIndex'
// format used to create standard clustered collections.
testInvalidCreateCollectionOptions({...options, clusteredIndex: {key: {_id: 1}, unique: true}}, [
    ErrorCodes.InvalidOptions,
]);

// Validate additional 'idIndex' field fails to create the collection when added to
// otherwise valid create options.
testInvalidCreateCollectionOptions({...options, idIndex: {key: {_id: 1}, name: "_id_"}}, ErrorCodes.InvalidOptions);

// Using the 'expireAfterSeconds' option on any namespace other than a time-series namespace or
// time-series buckets collection namespace should fail.
assert.commandFailedWithCode(testDB.createCollection("test", {expireAfterSeconds: 10}), ErrorCodes.InvalidOptions);
