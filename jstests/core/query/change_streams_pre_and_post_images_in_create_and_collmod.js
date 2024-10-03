/*
 * Tests that the 'changeStreamPreAndPostImages' option is settable via the collMod and create
 * commands. Also tests that this option cannot be set on view collections.
 * @tags: [
 * requires_fcv_60,
 * requires_replication,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent,
    assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled,
} from "jstests/libs/query/change_stream_util.js";

const dbName = 'testDB';
const collName = 'changeStreamPreAndPostImages';
const collName2 = 'changeStreamPreAndPostImages2';
const collName3 = 'changeStreamPreAndPostImages3';
const collName4 = 'changeStreamPreAndPostImages4';
const viewName = "view";

const testDB = db.getSiblingDB(dbName);
for (const collectionName of [collName, collName2, collName3, collName4, viewName]) {
    assertDropCollection(testDB, collectionName);
}

// Should be able to enable the 'changeStreamPreAndPostImages' via create or collMod.
assert.commandWorked(
    testDB.runCommand({create: collName, changeStreamPreAndPostImages: {enabled: true}}));
assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled(testDB, collName);

assert.commandWorked(testDB.runCommand({create: collName2}));
assert.commandWorked(
    testDB.runCommand({collMod: collName2, changeStreamPreAndPostImages: {enabled: true}}));
assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled(testDB, collName2);

// Verify that setting collection options with 'collMod' command does not affect
// 'changeStreamPreAndPostImages' option.
assert.commandWorked(testDB.runCommand({"collMod": collName2, validationLevel: "off"}));
assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled(testDB, collName2);

// Should successfully disable 'changeStreamPreAndPostImages' using the 'collMod' command.
assert.commandWorked(
    testDB.runCommand({collMod: collName2, changeStreamPreAndPostImages: {enabled: false}}));
assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent(testDB, collName2);

assert.commandWorked(testDB.runCommand({create: collName3}));

// Should fail to create a view with enabled 'changeStreamPreAndPostImages' option.
assert.commandFailedWithCode(
    testDB.runCommand(Object.assign(
        {create: viewName, viewOn: collName, changeStreamPreAndPostImages: {enabled: true}})),
    ErrorCodes.InvalidOptions);
assert.commandWorked(testDB.runCommand({create: viewName, viewOn: collName}));
assert.commandFailedWithCode(
    testDB.runCommand({collMod: viewName, changeStreamPreAndPostImages: {enabled: true}}),
    ErrorCodes.InvalidOptions);

// Should fail to create a timeseries collection with enabled 'changeStreamPreAndPostImages'
// option.
assert.commandFailedWithCode(testDB.runCommand({
    create: collName4,
    timeseries: {timeField: 'time'},
    changeStreamPreAndPostImages: {enabled: true}
}),
                             ErrorCodes.InvalidOptions);

// Creates a time-series collection with name 'collectionName' while using connection 'testDB'.
// Returns true if the collection was created, and false otherwise.
function createTimeSeriesCollection(collectionName) {
    const createCommandResponse =
        testDB.runCommand({create: collectionName, timeseries: {timeField: 'time'}});
    try {
        assert.commandWorked(createCommandResponse);
        return true;
    } catch (e) {
        // Verify that if in some passthroughs time-series collections are not allowed, then the
        // "create" command failed with InvalidOptions error.
        assert.commandFailedWithCode(
            createCommandResponse,
            ErrorCodes.InvalidOptions,
            "Time-series collection creation failed with an unexpected error");
        return false;
    }
}

if (createTimeSeriesCollection(collName4)) {
    // Should fail to enable 'changeStreamPreAndPostImages' option on a time-series collection.
    assert.commandFailedWithCode(
        testDB.runCommand({collMod: collName4, changeStreamPreAndPostImages: {enabled: true}}),
        ErrorCodes.InvalidOptions);
}