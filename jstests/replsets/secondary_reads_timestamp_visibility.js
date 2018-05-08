/**
 * Tests that reads on a secondary during batch application only see changes that occur at the last
 * applied timestamp, which is advanced at the completion of each batch.
 *
 * This test uses a failpoint to block right before batch application finishes, while holding the
 * PBWM lock, and before advancing the last applied timestamp for readers.
 *
 */
(function() {
    "use strict";

    load('jstests/replsets/libs/secondary_reads_test.js');

    const name = "secondaryReadsTimestampVisibility";
    const collName = "testColl";
    let secondaryReadsTest = new SecondaryReadsTest(name);
    let replSet = secondaryReadsTest.getReplset();

    let primaryDB = secondaryReadsTest.getPrimaryDB();
    let secondaryDB = secondaryReadsTest.getSecondaryDB();

    let primaryColl = primaryDB.getCollection(collName);

    // Create a collection and an index. Insert some data.
    primaryDB.runCommand({drop: collName});
    assert.commandWorked(primaryDB.runCommand({create: collName}));
    assert.commandWorked(primaryDB.runCommand(
        {createIndexes: collName, indexes: [{key: {y: 1}, name: "y_1", unique: true}]}));
    for (let i = 0; i < 100; i++) {
        assert.commandWorked(primaryColl.insert({_id: i, x: 0, y: i + 1}));
    }

    replSet.awaitReplication();

    // Sanity check.
    assert.eq(secondaryDB.getCollection(collName).find({x: 0}).itcount(), 100);
    assert.eq(secondaryDB.getCollection(collName).find({y: {$gte: 1, $lt: 101}}).itcount(), 100);

    // Prevent a batch from completing on the secondary.
    let pauseAwait = secondaryReadsTest.pauseSecondaryBatchApplication();

    // Update x to 1 in each document with default writeConcern and make sure we see the correct
    // data on the primary.
    let updates = [];
    for (let i = 0; i < 100; i++) {
        updates[i] = {q: {_id: i}, u: {x: 1, y: i}};
    }
    assert.commandWorked(primaryDB.runCommand({update: collName, updates: updates}));
    assert.eq(primaryColl.find({x: 1}).itcount(), 100);
    assert.eq(primaryColl.find({y: {$gte: 0, $lt: 100}}).itcount(), 100);

    // Wait for the batch application to pause.
    pauseAwait();

    let levels = ["local", "available", "majority"];

    // We should see the previous, un-replicated state on the secondary with every readconcern.
    for (let i in levels) {
        print("Checking that no new updates are visible yet for readConcern: " + levels[i]);
        assert.eq(secondaryDB.getCollection(collName).find({x: 0}).readConcern(levels[i]).itcount(),
                  100);
        assert.eq(secondaryDB.getCollection(collName).find({x: 1}).readConcern(levels[i]).itcount(),
                  0);
        assert.eq(secondaryDB.getCollection(collName)
                      .find({y: {$gte: 1, $lt: 101}})
                      .readConcern(levels[i])
                      .itcount(),
                  100);
    }

    // Disable the failpoint and let the batch complete.
    secondaryReadsTest.resumeSecondaryBatchApplication();

    replSet.awaitLastOpCommitted();

    for (let i in levels) {
        print("Checking that new updates are visible for readConcern: " + levels[i]);
        // We should see the new state on the secondary with every readconcern.
        assert.eq(secondaryDB.getCollection(collName).find({x: 0}).readConcern(levels[i]).itcount(),
                  0);
        assert.eq(secondaryDB.getCollection(collName).find({x: 1}).readConcern(levels[i]).itcount(),
                  100);
    }
    secondaryReadsTest.stop();
})();
