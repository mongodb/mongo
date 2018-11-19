/**
 * Test that operations with internalMajorityNoSnapshot write concern wait for operations to be
 * committed on a majority of nodes, but not in the majority snapshot.
 * @tags: [requires_majority_read_concern]
 */
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");
    load("jstests/replsets/rslib.js");

    const rst = new ReplSetTest({nodes: 2});
    if (!startSetIfSupportsReadMajority(rst)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        rst.stopSet();
        return;
    }

    rst.initiate();

    const dbName = "test";
    const collName = "write_concern_internal_majority_no_snapshot";
    const testDB = rst.getPrimary().getDB(dbName);
    const testColl = testDB[collName];

    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    //
    // Can be satisfied under normal conditions.
    //

    let docToInsert = {_id: 1};
    assert.commandWorked(testColl.runCommand({
        insert: collName,
        documents: [docToInsert],
        writeConcern: {w: "internalMajorityNoSnapshot"}
    }));

    // The document should be immediately visible locally and eventually visible in the committed
    // snapshot.
    assert.eq(docToInsert, testColl.findOne(docToInsert, null, null, "local"));
    assert.soon(
        () => friendlyEqual(docToInsert, testColl.findOne(docToInsert, null, null, "majority")));

    //
    // Can be satisfied without advancing the committed snapshot.
    //

    // Disable snapshotting so that future operations do not enter the majority snapshot.
    assert.commandWorked(rst.getPrimary().adminCommand(
        {configureFailPoint: "disableSnapshotting", mode: "alwaysOn"}));

    docToInsert = {_id: 2};
    assert.commandWorked(testColl.runCommand({
        insert: collName,
        documents: [docToInsert],
        writeConcern: {w: "internalMajorityNoSnapshot"}
    }));

    // The document should not be in the committed snapshot, but should be visible locally.
    assert.eq(null, testColl.findOne(docToInsert, null, null, "majority"));
    assert.eq(docToInsert, testColl.findOne(docToInsert, null, null, "local"));

    assert.commandWorked(
        rst.getPrimary().adminCommand({configureFailPoint: "disableSnapshotting", mode: "off"}));

    //
    // Cannot be satisfied without replication to a majority of voting nodes.
    //

    // Pause replication on the secondary so future operations cannot become majority committed.
    assert.commandWorked(
        rst.getSecondary().adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));

    // The write should succeed but the write concern should not be able to be satisfied.
    docToInsert = {_id: 3};
    const res = assert.commandWorkedIgnoringWriteConcernErrors(testColl.runCommand({
        insert: collName,
        documents: [docToInsert],
        writeConcern: {w: "internalMajorityNoSnapshot", wtimeout: 200}
    }));
    checkWriteConcernTimedOut(res);

    // The document should still be visible locally.
    assert.eq(docToInsert, testColl.findOne(docToInsert, null, null, "local"));

    assert.commandWorked(
        rst.getSecondary().adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));

    rst.stopSet();
}());
