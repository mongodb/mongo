// Test that $changeStreams usage is disallowed when the featureCompatibilityVersion is 3.4.
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const conn = rst.getPrimary();

    const testDB = conn.getDB("changeStreams_feature_compatibility_version");
    const coll = testDB.coll;
    assert.commandWorked(testDB.createCollection(coll.getName()));

    // Make sure $changeStreams works with (default) featureCompatibilityVersion 3.6

    assert.commandWorked(testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [{$changeStream: {}}], cursor: {}}));

    // $changeStreams is not permitted when the featureCompatibilityVersion is 3.4.

    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: coll.getName(), pipeline: [{$changeStream: {}}], cursor: {}}),
        ErrorCodes.InvalidOptions);

    rst.stopSet();
})();
