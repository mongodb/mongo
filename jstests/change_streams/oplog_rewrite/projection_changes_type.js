/**
 * Tests that a projection which retains expected fields but changes their types does not cause the
 * change stream framework to throw exceptions. Exercises the fix for SERVER-65497.
 * @tags: [ requires_fcv_60 ]
 */
(function() {
"use strict";

load("jstests/libs/change_stream_rewrite_util.js");  // For rewrite helpers.

const dbName = jsTestName();
const collName = "coll1";

// Establish a resume token at a point before anything actually happens in the test.
const startPoint = db.getMongo().watch().getResumeToken();

const testDB = db.getSiblingDB(dbName);
const numDocs = 8;

// Generate a write workload for the change stream to consume.
generateChangeStreamWriteWorkload(testDB, collName, numDocs);

// Obtain a list of all events that occurred during the write workload.
const fullEvents = getAllChangeStreamEvents(testDB, [], {showExpandedEvents: true}, startPoint);

// Traverse each of the events and build up a projection which empties objects and arrays, and
// changes the type of all scalar fields. Be sure to retain the _id field unmodified.
const computedProjection = {};
for (let event of fullEvents) {
    for (let fieldName in event) {
        if (fieldName === "_id" || computedProjection.hasOwnProperty(fieldName)) {
            continue;
        }
        const fieldVal = event[fieldName];
        if (Array.isArray(fieldVal)) {
            computedProjection[fieldName] = {$literal: []};
        } else if (isPlainObject(fieldVal)) {
            computedProjection[fieldName] = {$literal: {}};
        } else if (isNumber(fieldVal)) {
            computedProjection[fieldName] = {$literal: "dummy_value"};
        } else {
            computedProjection[fieldName] = {$literal: NumberInt(1)};
        }
    }
}

// Helper function which reads all change stream events, performs the specified projection on each,
// and confirms both that it succeeds and that all events in the original stream were observed.
function assertProjection(testProjection) {
    // Test both $project, which will exclude all but the specified computed fields, and $addFields,
    // which will retain all fields and overwrite the specified fields with the computed values.
    for (let projType of ["$project", "$addFields"]) {
        // Log the projection that we are about to test.
        jsTestLog(`Testing projection: ${tojsononeline({[projType]: testProjection})}`);

        // Read all events from the stream and apply the projection to each of them.
        const projectedEvents = getAllChangeStreamEvents(
            testDB, [{[projType]: testProjection}], {showExpandedEvents: true}, startPoint);

        // Assert that we see the same events in the projected stream as in the original.
        assert.eq(projectedEvents.map(elem => ({_id: elem._id})),
                  fullEvents.map(elem => ({_id: elem._id})));
    }
}

// Extract the list of fields that we observed in the stream. We randomize the order of the array so
// that, over time, every combination of fields will be tested without having to generate a power
// set each time this test runs.
const gitHash = assert.commandWorked(testDB.runCommand("buildInfo")).gitVersion;
Random.setRandomSeed(Number("0x" + gitHash.substring(0, 13)));  // max 2^52-1
const fieldsToInclude = Object.keys(computedProjection)
                            .map(value => ({value, sort: Random.rand()}))
                            .sort((a, b) => a.sort - b.sort)
                            .map(({value}) => value);

// Now iterate through each field and test both projection of that single field, and projection of
// all accumulated fields encountered so far.
const accumulatedProjection = {};
for (let fieldName of fieldsToInclude) {
    // Test projection of this single field.
    const currentFieldProjection = {[fieldName]: computedProjection[fieldName]};
    assertProjection(currentFieldProjection);

    // Test projection of all accumulated fields.
    assertProjection(Object.assign(accumulatedProjection, currentFieldProjection));
}
})();
