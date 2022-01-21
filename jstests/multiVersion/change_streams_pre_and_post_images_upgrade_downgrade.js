/**
 * Verifies that it is possible to upgrade a replica set with collections with 'recordPreImages'
 * option to use 'changeStreamPreAndPostImages' option, and to do a corresponding downgrade.
 * @tags: [
 * requires_fcv_52,
 * featureFlagChangeStreamPreAndPostImages,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/collection_drop_recreate.js");  // For assertCreateCollection.
load("jstests/libs/collection_options.js");        // For assertCollectionOptionIsEnabled,
                                                   // assertCollectionOptionIsAbsent.
load("jstests/multiVersion/libs/multi_rs.js");     // For upgradeSet.
load(
    "jstests/libs/change_stream_util.js");  // For
                                            // assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled,
                                            // assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent.
                                            // assertPreImagesCollectionIsAbsent,
                                            // assertPreImagesCollectionExists.
load("jstests/libs/fail_point_util.js");  // For configureFailPoint.

const collName = "test";
const latestBinVersion = "latest";

// Checks that the pre-image of the next change event in the change stream equals to the
// 'expectedPreImage'.
function assertNextPreImage(changeStream, expectedPreImage) {
    assert.soon(() => changeStream.hasNext());
    assert.eq(changeStream.next().fullDocumentBeforeChange, expectedPreImage);
}

// Tests "changeStreamPreAndPostImages" option for the "create" and "collMod" commands in downgraded
// and upgraded FCV states. Tests an FCV downgrade succeeds when no collection with
// changeStreamPreImages: {enabled: true} exists.
function testCreateAndCollModCommandsInUpgradedDowngradedFCVStates(downgradeFCV) {
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: downgradeFCV},
    });
    rst.startSet();
    rst.initiate();

    // Create the collection with recorded pre-images enabled.
    let testDB = rst.getPrimary().getDB(jsTestName());
    assertCreateCollection(testDB, collName, {"recordPreImages": true});

    // Upgrade the replica set.
    rst.upgradeSet({binVersion: latestBinVersion});
    testDB = rst.getPrimary().getDB(jsTestName());

    // Verify that an attempt to set 'changeStreamPreAndPostImages' option fails for the downgraded
    // FCV version.
    assert.commandFailedWithCode(
        testDB.createCollection("anotherTestCollection",
                                {"changeStreamPreAndPostImages": {enabled: false}}),
        ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        testDB.runCommand({"collMod": collName, "changeStreamPreAndPostImages": {enabled: false}}),
        ErrorCodes.InvalidOptions);

    // Set the FCV to the latest.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // 'changeStreamPreAndPostImages' option must be absent and 'recordPreImages' option must be set
    // to true.
    assertCollectionOptionIsEnabled(testDB, collName, "recordPreImages");
    assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent(testDB, collName);

    // Enable pre-/post-images for the collection with "recordPreImages" enabled.
    assert.commandWorked(
        testDB.runCommand({"collMod": collName, "changeStreamPreAndPostImages": {enabled: true}}));

    // 'changeStreamPreAndPostImages' option must be enabled and 'recordPreImages' should be absent.
    assertCollectionOptionIsAbsent(testDB, collName, "recordPreImages");
    assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled(testDB, collName);

    // Set 'recordPreImages: true' to disable 'changeStreamPreAndPostImages' option.
    assert.commandWorked(testDB.runCommand({"collMod": collName, "recordPreImages": true}));

    // 'changeStreamPreAndPostImages' option must be absent and 'recordPreImages' should be set to
    // true.
    assertCollectionOptionIsEnabled(testDB, collName, "recordPreImages");
    assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent(testDB, collName);

    // Downgrade the FCV.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // Downgrade the replica set.
    rst.upgradeSet({binVersion: downgradeFCV});
    rst.stopSet();
}

// Tests that when change stream pre-images are recorded on a collection using option
// recordPreImages: true and, after FCV upgrade, recordPreImages: true option is replaced with
// changeStreamPreAndPostImages: {enabled: true} , then pre-images are available for all change
// events in a change stream without interruption. Subsequently, tests a FCV downgrade and switching
// back from option changeStreamPreAndPostImages: {enabled: true} to recordPreImages: true.
function testUpgradeDowngradeFromRecordPreImageOptionToChangeStreamPreAndPostImages(downgradeFCV) {
    // Upgrade scenario.
    // Upgrade server binary.
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: downgradeFCV},
    });
    rst.startSet();
    rst.initiate();
    rst.upgradeSet({binVersion: latestBinVersion});

    // Create the collection with recorded pre-images enabled and insert one document.
    const testDB = rst.getPrimary().getDB(jsTestName());
    const coll = assertCreateCollection(testDB, collName, {recordPreImages: true});
    assert.commandWorked(coll.insert({_id: 1, eventId: 1}));

    // Open a change stream with fullDocumentBeforeChange: "required".
    const changeStream = coll.watch([], {fullDocumentBeforeChange: "required"});

    // Perform an "update" command. Pre-image will be recorded in the oplog.
    assert.commandWorked(coll.update({_id: 1}, {$inc: {eventId: 1}}));

    // Upgrade to the latest FCV.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Verify that the pre-images collection is created.
    assertPreImagesCollectionExists(testDB);

    // Enable change stream pre-images recording for the collection.
    assert.commandWorked(
        testDB.runCommand({collMod: collName, changeStreamPreAndPostImages: {enabled: true}}));

    // Issue an "update" command for which the pre-image won't be available after the FCV downgrade.
    assert.commandWorked(coll.update({_id: 1}, {$inc: {eventId: 1}}));

    // Verify that change stream receives change event with pre-image being set.
    assertNextPreImage(changeStream, {_id: 1, eventId: 1});

    // Issue an "update" command for which the pre-image won't be available after the FCV downgrade.
    assert.commandWorked(coll.update({_id: 1}, {$inc: {eventId: 1}}));

    // Downgrade scenario.
    // Revert to the previous pre-image recording capability available in 5.0.
    assert.commandWorked(testDB.runCommand({collMod: collName, recordPreImages: true}));

    // Verify that the change stream returns a change event with a pre-image set.
    assertNextPreImage(changeStream, {_id: 1, eventId: 2});

    // Downgrade the FCV version. Pre-images collection is dropped during the downgrade.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // Verify that pre-images collection is dropped.
    assertPreImagesCollectionIsAbsent(testDB);

    // Verify that reading the next change event fails for change stream with
    // fullDocumentBeforeChange: "required", as pre-image for this event was recorded in the
    // pre-images collection.
    assert.throwsWithCode(() => changeStream.hasNext(), ErrorCodes.NoMatchingDocument);

    rst.stopSet();
}

// Tests that when change stream pre-images are recorded on a collection using option
// changeStreamPreAndPostImages: {enabled: true} and, after changeStreamPreAndPostImages option is
// disabled, then pre-images are unavailable to change stream change events after FCV downgrade.
function testDowngrade(downgradeFCV) {
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: latestBinVersion},
    });
    rst.startSet();
    rst.initiate();

    // Create the collection with changeStreamPreAndPostImages: {enabled: true} and perform insert
    // and update operations.
    const testDB = rst.getPrimary().getDB(jsTestName());
    const coll =
        assertCreateCollection(testDB, collName, {changeStreamPreAndPostImages: {enabled: true}});
    const changeStream = coll.watch([], {fullDocumentBeforeChange: "required"});
    assert.commandWorked(coll.insert({_id: 1, eventId: 1}));
    assert.commandWorked(coll.update({_id: 1}, {$inc: {eventId: 1}}));

    // Downgrade scenario.
    // Issue "collMod" command in order to disable changeStreamPreAndPostImages option.
    assert.commandWorked(
        testDB.runCommand({"collMod": collName, changeStreamPreAndPostImages: {enabled: false}}));

    // Downgrade the FCV version.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // Verify that the pre-images collection is dropped.
    assertPreImagesCollectionIsAbsent(testDB);

    // Verify that reading the next change event fails for change stream with
    // fullDocumentBeforeChange: "required", as pre-image for this event was recorded in the
    // pre-images collection that no longer exists.
    assert.throwsWithCode(() => changeStream.hasNext(), ErrorCodes.NoMatchingDocument);

    rst.stopSet();
}

// Tests that downgrading of the FCV fails if there exists a collection with
// changeStreamPreAndPostImages: {enabled: true}.
function testFCVDowngradeFailureWhenChangeStreamPreAndPostImagesEnabledForCollection(downgradeFCV) {
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: latestBinVersion},
    });
    rst.startSet();
    rst.initiate();
    const testDB = rst.getPrimary().getDB(jsTestName());

    // Pre-images collection must exist upon start-up with the latest FCV.
    assertPreImagesCollectionExists(testDB);
    assert.commandWorked(
        testDB.createCollection("testCollection", {changeStreamPreAndPostImages: {enabled: true}}));

    // Verify that a downgrade of the FCV fails when there is at least one collection with
    // {changeStreamPreAndPostImages: {enabled: true}} option set.
    assert.commandFailedWithCode(
        testDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}),
        ErrorCodes.CannotDowngrade);

    // Verify that the pre-images collection is not dropped in case of a failed FCV downgrade.
    assertPreImagesCollectionExists(testDB);

    rst.stopSet();
}

// Tests that FCV upgrade fails if there is an error creating pre-images collection.
function testFCVUpgradeFailureWhenCreationOfPreImagesCollectionFails(downgradeFCV) {
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: binVersionFromFCV(downgradeFCV)},
    });
    rst.startSet();
    rst.initiate();
    rst.upgradeSet({binVersion: latestBinVersion});
    const testDB = rst.getPrimary().getDB(jsTestName());
    configureFailPoint(rst.getPrimary(), "failPreimagesCollectionCreation", {}, {times: 1});

    // Verify that FCV upgrade fails when creation of the pre-images collection fails.
    assert.commandFailedWithCode(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}),
                                 5868501);

    // Verfiy that FCV version remains unchanged.
    const fcvDoc = testDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.eq(fcvDoc.featureCompatibilityVersion.version, downgradeFCV, fcvDoc);

    // Verify that the pre-images collection is not created.
    assertPreImagesCollectionIsAbsent(testDB);

    rst.stopSet();
}

runFeatureFlagMultiversionTest('featureFlagChangeStreamPreAndPostImages',
                               testCreateAndCollModCommandsInUpgradedDowngradedFCVStates);
runFeatureFlagMultiversionTest(
    'featureFlagChangeStreamPreAndPostImages',
    testUpgradeDowngradeFromRecordPreImageOptionToChangeStreamPreAndPostImages);
runFeatureFlagMultiversionTest('featureFlagChangeStreamPreAndPostImages', testDowngrade);
runFeatureFlagMultiversionTest(
    'featureFlagChangeStreamPreAndPostImages',
    testFCVDowngradeFailureWhenChangeStreamPreAndPostImagesEnabledForCollection);
runFeatureFlagMultiversionTest('featureFlagChangeStreamPreAndPostImages',
                               testFCVUpgradeFailureWhenCreationOfPreImagesCollectionFails);
})();
