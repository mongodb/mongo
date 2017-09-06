// Test that $changeStreams usage is disallowed when the featureCompatibilityVersion is 3.4.
// and that existing streams close when FCV is set to 3.4
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

    const rst = new ReplSetTest({nodes: 1, nodeOptions: {enableMajorityReadConcern: ""}});
    if (!startSetIfSupportsReadMajority(rst)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }
    rst.initiate();
    const conn = rst.getPrimary();

    const testDB = conn.getDB("changeStreams_feature_compatibility_version");
    const coll = testDB.coll;
    assert.commandWorked(testDB.createCollection(coll.getName()));

    // Open a changeStream with (default) featureCompatibilityVersion 3.6.

    let res = assert.commandWorked(testDB.runCommand(
        {aggregate: coll.getName(), pipeline: [{$changeStream: {}}], cursor: {}}));

    // Make sure we can get a change from it.
    assert.writeOK(coll.insert({_id: 0}));
    res = assert.commandWorked(
        testDB.runCommand({getMore: res.cursor.id, collection: coll.getName()}));
    assert.neq(0, res.cursor.id);
    assert.eq(1, res.cursor.nextBatch.length);

    // Change stream should close when we read an oplog entry written with
    // featureCompatibilityVersion 3.4.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    assert.writeOK(coll.insert({_id: 1}));
    res = assert.commandWorked(
        testDB.runCommand({getMore: res.cursor.id, collection: coll.getName()}));
    assert.eq(0, res.cursor.id);
    assert.eq(1, res.cursor.nextBatch.length);
    assert.eq("invalidate", res.cursor.nextBatch[0].operationType);

    // Opening new $changeStreams is not permitted when the featureCompatibilityVersion is 3.4.
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: coll.getName(), pipeline: [{$changeStream: {}}], cursor: {}}),
        ErrorCodes.InvalidOptions);

    rst.stopSet();
})();
