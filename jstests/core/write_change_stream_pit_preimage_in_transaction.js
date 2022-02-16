/**
 * Tests that pre-images are written to the pre-images collection on updates and deletes in
 * transactions and for non-atomic "applyOps" command.
 * @tags: [
 *  requires_fcv_53,
 *  featureFlagChangeStreamPreAndPostImages,
 *  assumes_against_mongod_not_mongos,
 *  requires_capped,
 *  requires_replication,
 *  requires_getmore,
 *  uses_transactions,
 *  no_selinux,
 * ]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");  // For PrepareHelpers.prepareTransaction.
load("jstests/libs/fixture_helpers.js");            // For FixtureHelpers.isReplSet().
load("jstests/libs/collection_drop_recreate.js");   // For assertDropAndRecreateCollection.
load(
    "jstests/libs/change_stream_util.js");  // For
                                            // assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled,
                                            // assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent,
                                            // preImagesForOps.
load("jstests/libs/transactions_util.js");  // For TransactionsUtil.runInTransaction.

// TODO SERVER-63272: remove this check.
if (!FixtureHelpers.isReplSet(db)) {
    jsTestLog(
        "Skipping the test as pre-images are not recorded in standalone mode and the test is designed to work with a replica set.");
    return;
}

const testDB = db.getSiblingDB(jsTestName());
const localDB = db.getSiblingDB("local");

// Verifies that the expected pre-images are written during function 'ops' invocation.
function assertPreImagesWrittenForOps(db, ops, expectedPreImages) {
    const writtenPreImages = preImagesForOps(db, ops);
    assert.eq(
        expectedPreImages.length,
        writtenPreImages.length,
        `Expected pre-image documents: ${tojson(expectedPreImages)}. Found pre-image documents: ${
            tojson(writtenPreImages)}.`);

    for (let idx = 0; idx < writtenPreImages.length; idx++) {
        assert.eq(writtenPreImages[idx].preImage, expectedPreImages[idx]);
        assertValidChangeStreamPreImageDocument(writtenPreImages[idx]);
    }
}

// Cross-checks the content of the pre-image document 'preImage' against the associated oplog entry.
function assertValidChangeStreamPreImageDocument(preImage) {
    function assertChangeStreamPreImageDocumentMatchesOplogEntry(oplogEntry, preImage, wallTime) {
        // Pre-images documents are recorded only for update and delete commands.
        assert.contains(oplogEntry.op, ["u", "d"], oplogEntry);
        assert.eq(preImage._id.nsUUID, oplogEntry.ui, oplogEntry);
        assert.eq(preImage.operationTime, wallTime, oplogEntry);
        if (oplogEntry.hasOwnProperty("o2")) {
            assert.eq(preImage.preImage._id, oplogEntry.o2._id, oplogEntry);
        }
    }
    const oplogEntryCursor = localDB.oplog.rs.find({ts: preImage._id.ts});
    assert(oplogEntryCursor.hasNext());
    const oplogEntry = oplogEntryCursor.next();
    if (oplogEntry.o.hasOwnProperty("applyOps")) {
        const applyOpsOplogEntry = oplogEntry;
        assert(preImage._id.applyOpsIndex < applyOpsOplogEntry.o.applyOps.length);
        const applyOpsEntry = applyOpsOplogEntry.o.applyOps[preImage._id.applyOpsIndex.toNumber()];
        assertChangeStreamPreImageDocumentMatchesOplogEntry(
            applyOpsEntry, preImage, applyOpsOplogEntry.wall);
    } else {
        assert.eq(preImage._id.applyOpsIndex,
                  0,
                  "applyOpsIndex value greater than 0 not expected for non-applyOps oplog entries");
        assertChangeStreamPreImageDocumentMatchesOplogEntry(oplogEntry, preImage, oplogEntry.wall);
    }
}

const coll = assertDropAndRecreateCollection(
    testDB, "coll", {changeStreamPreAndPostImages: {enabled: true}});
const otherColl = assertDropAndRecreateCollection(testDB, "coll_regular");

// Gets collections used in the test for database 'db'. In some passthroughs the collections get
// sharded on 'getCollection()' invocation and it must happen when a transaction is not active.
function getCollections(db) {
    return {coll: db[coll.getName()], otherColl: db[otherColl.getName()]};
}

// Tests the pre-image writing behavior in a transaction.
(function testPreImageWritingInTransaction() {
    // Verify that the pre-images are written correctly for a transaction with update and delete
    // operations consisting of a single "applyOps" entry.
    assert.commandWorked(coll.insert([{_id: 1, a: 1}, {_id: 2, a: 1}, {_id: 3, a: 1}]));
    assert.commandWorked(otherColl.insert([{_id: 1, a: 1}]));
    assertPreImagesWrittenForOps(db, function() {
        TransactionsUtil.runInTransaction(testDB, getCollections, function(db, {coll, otherColl}) {
            assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {a: 1}}));
            assert.commandWorked(otherColl.updateOne({_id: 1}, {$inc: {a: 1}}));
            assert.commandWorked(coll.updateOne({_id: 2}, {$inc: {a: 1}}));
            assert.commandWorked(coll.deleteOne({_id: 3}));
        });
    }, [{_id: 1, a: 1}, {_id: 2, a: 1}, {_id: 3, a: 1}]);

    // Verify that the pre-images are written correctly for a transaction with update and delete
    // operations consisting of multiple "applyOps" entries.
    const stringSizeInBytes = 15 * 1024 * 1024;
    const largeString = "b".repeat(stringSizeInBytes);
    assert.commandWorked(coll.insert([{_id: 3, a: 1}]));
    assertPreImagesWrittenForOps(db, function() {
        TransactionsUtil.runInTransaction(testDB, getCollections, function(db, {coll, otherColl}) {
            assert.commandWorked(otherColl.insert({b: largeString}));
            assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {a: 1}}));

            assert.commandWorked(otherColl.insert({b: largeString}));
            assert.commandWorked(coll.updateOne({_id: 2}, {$inc: {a: 1}}));
            assert.commandWorked(coll.deleteOne({_id: 3}));
        });
    }, [{_id: 1, a: 2}, {_id: 2, a: 2}, {_id: 3, a: 1}]);

    // Verify that large pre-images are written correctly for a transaction.
    assert.commandWorked(coll.insert([{_id: 3, a: largeString}]));
    assertPreImagesWrittenForOps(db, function() {
        TransactionsUtil.runInTransaction(testDB, getCollections, function(db, {coll, _}) {
            assert.commandWorked(coll.updateOne({_id: 1}, {$set: {b: largeString}}));
            assert.commandWorked(coll.deleteOne({_id: 3}));
            assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {a: 1}}));
        });
    }, [{_id: 1, a: 3}, {_id: 3, a: largeString}, {_id: 1, a: 3, b: largeString}]);
})();

(function testPreImageWritingForApplyOpsCommand() {
    assert.commandWorked(coll.deleteMany({}));
    assert.commandWorked(coll.insert([{_id: 1, a: 1}, {_id: 2, a: 1}]));

    // Verify that pre-images are written correctly for the non-atomic "applyOps" command.
    assertPreImagesWrittenForOps(db, function() {
        assert.commandWorked(testDB.runCommand({
            applyOps: [
                {op: "u", ns: coll.getFullName(), o2: {_id: 1}, o: {$set: {a: 2}}},
                {op: "d", ns: coll.getFullName(), o: {_id: 2}}
            ],
            allowAtomic: false,
        }));
    }, [{_id: 1, a: 1}, {_id: 2, a: 1}]);
})();

(function testPreImageWritingForPreparedTransaction() {
    assert.commandWorked(coll.deleteMany({}));
    assert.commandWorked(coll.insert([{_id: 1, a: 1}, {_id: 3, a: 1}]));

    // Verify that pre-images are written correctly for a transaction that is prepared and then
    // committed.
    assertPreImagesWrittenForOps(db, function() {
        const session = db.getMongo().startSession();
        const sessionDb = session.getDatabase(jsTestName());
        session.startTransaction({readConcern: {level: "majority"}});
        const collInner = sessionDb[coll.getName()];
        assert.commandWorked(collInner.updateOne({_id: 1}, {$inc: {a: 1}}));
        assert.commandWorked(collInner.deleteOne({_id: 3}));
        let prepareTimestamp = PrepareHelpers.prepareTransaction(session);
        assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
    }, [{_id: 1, a: 1}, {_id: 3, a: 1}]);
})();

(function testPreImageWritingForAbortedPreparedTransaction() {
    assert.commandWorked(coll.deleteMany({}));
    assert.commandWorked(coll.insert([{_id: 1, a: 1}, {_id: 3, a: 1}]));

    // Verify that pre-images are not written for a transaction that is prepared and then aborted.
    assertPreImagesWrittenForOps(db, function() {
        const session = db.getMongo().startSession();
        const sessionDb = session.getDatabase(jsTestName());
        session.startTransaction({readConcern: {level: "majority"}});
        const collInner = sessionDb[coll.getName()];
        assert.commandWorked(collInner.updateOne({_id: 1}, {$inc: {a: 1}}));
        assert.commandWorked(collInner.deleteOne({_id: 3}));
        PrepareHelpers.prepareTransaction(session);
        assert.commandWorked(session.abortTransaction_forTesting());
    }, []);
})();
}());
