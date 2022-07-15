/*
 * Tests that the 'changeStreamPreAndPostImages' option is settable via the collMod and create
 * commands. Also tests that this option cannot be set on collections in the 'local', 'admin',
 * 'config' databases as well as on view collections.
 * @tags: [
 * requires_fcv_60,
 * requires_replication,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/collection_options.js");  // For assertCollectionOptionIsEnabled,
                                             // assertCollectionOptionIsAbsent.
load(
    "jstests/libs/change_stream_util.js");  // For
                                            // assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled,
                                            // assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent.

const rsTest = new ReplSetTest({name: jsTestName(), nodes: 1});
rsTest.startSet();
rsTest.initiate();

const dbName = 'testDB';
const collName = 'changeStreamPreAndPostImages';
const collName2 = 'changeStreamPreAndPostImages2';
const collName3 = 'changeStreamPreAndPostImages3';
const collName4 = 'changeStreamPreAndPostImages4';
const collName5 = 'changeStreamPreAndPostImages5';
const viewName = "view";

const primary = rsTest.getPrimary();
const adminDB = primary.getDB("admin");
const localDB = primary.getDB("local");
const configDB = primary.getDB("config");
const testDB = primary.getDB(dbName);

// Check that we cannot set 'changeStreamPreAndPostImages' on the local, admin and config databases.
for (const db of [localDB, adminDB, configDB]) {
    assert.commandFailedWithCode(
        db.runCommand({create: collName, changeStreamPreAndPostImages: {enabled: true}}),
        ErrorCodes.InvalidOptions);

    assert.commandWorked(db.runCommand({create: collName}));
    assert.commandFailedWithCode(
        db.runCommand({collMod: collName, changeStreamPreAndPostImages: {enabled: true}}),
        ErrorCodes.InvalidOptions);
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

// Both 'recordPreImages' and 'changeStreamPreAndPostImages' may not be enabled at the same
// time.
assert.commandFailedWithCode(
    testDB.runCommand(
        {collMod: collName3, recordPreImages: true, changeStreamPreAndPostImages: {enabled: true}}),
    ErrorCodes.InvalidOptions);

assert.commandWorked(testDB.runCommand({create: collName3}));
assert.commandFailedWithCode(
    testDB.runCommand(
        {collMod: collName3, recordPreImages: true, changeStreamPreAndPostImages: {enabled: true}}),
    ErrorCodes.InvalidOptions);

// Should set 'recordPreImages' to true and disable 'changeStreamPreAndPostImages' option.
assert.commandWorked(testDB.runCommand(
    {collMod: collName3, recordPreImages: true, changeStreamPreAndPostImages: {enabled: false}}));
assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent(testDB, collName3);
assertCollectionOptionIsEnabled(testDB, collName3, "recordPreImages");

// Should set 'recordPreImages' to false and enable 'changeStreamPreAndPostImages'.
assert.commandWorked(testDB.runCommand(
    {collMod: collName3, recordPreImages: false, changeStreamPreAndPostImages: {enabled: true}}));
assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled(testDB, collName3);
assertCollectionOptionIsAbsent(testDB, collName3, "recordPreImages");

// Set 'recordPreImages: true' to disable 'changeStreamPreAndPostImages' option.
assert.commandWorked(testDB.runCommand({"collMod": collName3, "recordPreImages": true}));

// 'changeStreamPreAndPostImages' option must be absent and 'recordPreImages' should be set to
// true.
assertCollectionOptionIsEnabled(testDB, collName3, "recordPreImages");
assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent(testDB, collName3);

// Enable pre-/post-images for the collection with 'changeStreamPreAndPostImages' enabled.
// Set 'changeStreamPreAndPostImages: {enabled: true}' to disable 'recordPreImages' option.
assert.commandWorked(
    testDB.runCommand({collMod: collName3, changeStreamPreAndPostImages: {enabled: true}}));

// 'changeStreamPreAndPostImages' option must be enabled and 'recordPreImages' should be
// absent.
assertCollectionOptionIsAbsent(testDB, collName3, "recordPreImages");
assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled(testDB, collName3);

// Should set 'recordPreImages' to true and disable 'changeStreamPreAndPostImages' option.
assert.commandWorked(testDB.runCommand(
    {create: collName4, recordPreImages: true, changeStreamPreAndPostImages: {enabled: false}}));
assert.commandWorked(testDB.runCommand(
    {collMod: collName4, recordPreImages: true, changeStreamPreAndPostImages: {enabled: false}}));
assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent(testDB, collName4);

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
    create: collName5,
    timeseries: {timeField: 'time'},
    changeStreamPreAndPostImages: {enabled: true}
}),
                             ErrorCodes.InvalidOptions);

// Should fail to enable 'changeStreamPreAndPostImages' option on a timeseries collection.
assert.commandWorked(testDB.runCommand({create: collName5, timeseries: {timeField: 'time'}}));
assert.commandFailedWithCode(
    testDB.runCommand({collMod: collName5, changeStreamPreAndPostImages: {enabled: true}}),
    ErrorCodes.InvalidOptions);

rsTest.stopSet();
}());
