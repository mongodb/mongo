// Test initial sync with documents moving forward.
//
// This tests that initial sync succeeds when the clone phase encounters the same _id twice. We test
// that the destination node has the correct document with that _id at the end of initial sync.
//
// We also test that the initial sync succeeds when the clone phase encounters the same 'x' value
// twice, for a collection with a unique index {x: 1}.
//
// It works by deleting a document at the end of the range we are cloning, then growing a document
// from the beginning of the range so that it moves to the hole in the end of the range.
//
// This also works for wiredTiger, because we grow the document by deleting and reinserting it, so
// the newly inserted document is included in the cursor on the source.
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    var rst = new ReplSetTest({name: "initial_sync_move_forward", nodes: 1});
    rst.startSet();
    rst.initiate();

    var masterColl = rst.getPrimary().getDB("test").coll;

    // Insert 500000 documents. Make the last two documents larger, so that {_id: 0, x: 0} and {_id:
    // 1, x: 1} will fit into their positions when we grow them.
    var count = 500000;
    var bulk = masterColl.initializeUnorderedBulkOp();
    for (var i = 0; i < count - 2; ++i) {
        bulk.insert({_id: i, x: i});
    }
    var longString = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    bulk.insert({_id: count - 2, x: count - 2, longString: longString});
    bulk.insert({_id: count - 1, x: count - 1, longString: longString});
    assert.writeOK(bulk.execute());

    // Create a unique index on {x: 1}.
    assert.commandWorked(masterColl.ensureIndex({x: 1}, {unique: true}));

    // Add a secondary.
    var secondary = rst.add({setParameter: "num3Dot2InitialSyncAttempts=1"});
    secondary.setSlaveOk();
    var secondaryColl = secondary.getDB("test").coll;

    // Pause initial sync when the secondary has copied {_id: 0, x: 0} and {_id: 1, x: 1}.
    assert.commandWorked(secondary.adminCommand({
        configureFailPoint: "initialSyncHangDuringCollectionClone",
        data: {namespace: secondaryColl.getFullName(), numDocsToClone: 2},
        mode: "alwaysOn"
    }));
    rst.reInitiate();
    assert.soon(function() {
        var logMessages = assert.commandWorked(secondary.adminCommand({getLog: "global"})).log;
        for (var i = 0; i < logMessages.length; i++) {
            if (logMessages[i].indexOf(
                    "initial sync - initialSyncHangDuringCollectionClone fail point enabled") !=
                -1) {
                return true;
            }
        }
        return false;
    });

    // Delete {_id: count - 2} to make a hole. Grow {_id: 0} so that it moves into that hole. This
    // will cause the secondary to clone {_id: 0} again.
    // Change the value for 'x' so that we are not testing the uniqueness of 'x' in this case.
    assert.writeOK(masterColl.remove({_id: 0, x: 0}));
    assert.writeOK(masterColl.remove({_id: count - 2, x: count - 2}));
    assert.writeOK(masterColl.insert({_id: 0, x: count, longString: longString}));

    // Delete {_id: count - 1} to make a hole. Grow {x: 1} so that it moves into that hole. This
    // will cause the secondary to clone {x: 1} again.
    // Change the value for _id so that we are not testing the uniqueness of _id in this case.
    assert.writeOK(masterColl.remove({_id: 1, x: 1}));
    assert.writeOK(masterColl.remove({_id: count - 1, x: count - 1}));
    assert.writeOK(masterColl.insert({_id: count, x: 1, longString: longString}));

    // Resume initial sync.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

    // Wait for initial sync to finish.
    rst.awaitSecondaryNodes();

    // Check document count on secondary.
    assert.eq(count - 2, secondaryColl.find().itcount());

    // Check for {_id: 0} on secondary.
    assert.eq(1, secondaryColl.find({_id: 0, x: count}).itcount());

    // Check for {x: 1} on secondary.
    assert.eq(1, secondaryColl.find({_id: count, x: 1}).itcount());

    // Check for unique index on secondary.
    var indexSpec = GetIndexHelpers.findByKeyPattern(secondaryColl.getIndexes(), {x: 1});
    assert.neq(null, indexSpec);
    assert.eq(true, indexSpec.unique);
})();