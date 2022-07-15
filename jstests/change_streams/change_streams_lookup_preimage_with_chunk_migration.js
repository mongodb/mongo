/**
 * Tests that the pre-images are not recorded in 'system.preimages' collection when the request to
 * update or delete an image comes from the chunk migration event.
 *
 *  @tags: [
 *    requires_fcv_60,
 *    requires_sharding,
 *    uses_change_streams,
 *    change_stream_does_not_expect_txns,
 *    assumes_unsharded_collection,
 *    assumes_read_preference_unchanged,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load("jstests/libs/chunk_manipulation_util.js");   // For pauseMigrateAtStep, waitForMigrateStep and
                                                   // unpauseMigrateAtStep.

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const dbName = jsTestName();
const collName = "test";
const mongosConn = st.s;
const db = mongosConn.getDB(dbName);

const donor = st.shard0;
const recipient = st.shard1;

// Creates a sharded collection and enables recording of pre-images for it. Returns the sharded
// collection.
const coll = (() => {
    assertDropAndRecreateCollection(db, collName);

    st.ensurePrimaryShard(dbName, donor.shardName);

    const coll = db.getCollection(collName);

    // Allow 'system.preimages' collection to record pre-images for the specified collection. Ensure
    // that the recording is actually enabled for the collection.
    assert.commandWorked(
        db.runCommand({collMod: collName, changeStreamPreAndPostImages: {enabled: true}}));
    assert(db.getCollectionInfos({name: collName})[0].options.changeStreamPreAndPostImages.enabled);

    // Shard the collection based on '_id'. Split chunk at '_id: 1'.
    st.shardColl(
        collName, {_id: 1} /* shard key */, {_id: 1} /* split at */, false /* move */, dbName);

    return coll;
})();

// Verifies that expected 'fromMigrate' events are observed in the oplog for the specified shard.
function verifyFromMigrateOplogEvents(shard, docId, ops) {
    const oplogEvents = shard.getDB("local")
                            .getCollection("oplog.rs")
                            .find({"fromMigrate": true, "o._id": docId})
                            .toArray();
    assert.eq(oplogEvents.length, ops.length, oplogEvents);

    for (let idx = 0; idx < oplogEvents.length; idx++) {
        assert.eq(oplogEvents[idx].op, ops[idx], oplogEvents[idx]);
        assert.eq(oplogEvents[idx].o._id, docId, oplogEvents[idx]);
    }
}

// Verifies that expected pre-images are stored in the pre-image collection for the specified shard.
function verifyPreImages(shard, docId, annotations) {
    const foundPreImages = shard.getDB("config")
                               .getCollection("system.preimages")
                               .find({"preImage._id": docId})
                               .toArray();
    assert.eq(foundPreImages.length, annotations.length, foundPreImages);

    for (let idx = 0; idx < foundPreImages.length; idx++) {
        assert.eq(foundPreImages[idx].preImage._id, docId, foundPreImages[idx].preImage);
        assert.eq(
            foundPreImages[idx].preImage.annotate, annotations[idx], foundPreImages[idx].preImage);
    }
}

// Verifies that the change streams cursor sees the required events.
function verifyChangeStreamEvents(csCursor, events) {
    events.forEach(expEvent => {
        assert.soon(() => csCursor.hasNext());
        const event = csCursor.next();

        assert.eq(event.operationType, expEvent.opType);

        if (event.operationType == "insert") {
            assert.eq(event.documentKey._id, expEvent.id, event);
        } else if (event.operationType == "update") {
            assert.eq(event.documentKey._id, expEvent.id, event);
            assert.eq(event.fullDocumentBeforeChange.annotate, expEvent.prevAnnotate, event);
            assert.eq(event.fullDocument.annotate, expEvent.curAnnotate, event);
        } else if (event.operationType == "delete") {
            assert.eq(event.documentKey._id, expEvent.id, event);
            assert.eq(event.fullDocumentBeforeChange.annotate, expEvent.prevAnnotate, event);
        }
    });
}

// Tests that pre-images are recorded correctly when run sequentially with the chunk-migration.
(function testSerialUpdateAndMoveChunk() {
    // Open change streams here to record all events in the collection.
    const csCursor =
        coll.watch([], {fullDocument: "required", fullDocumentBeforeChange: "required"});

    // Insert 1 document to the collection.
    assert.commandWorked(coll.insert({_id: 0, annotate: "before_update"}));

    // Insert operation should not insert any document to the pre-image collection.
    verifyPreImages(donor, 0, []);
    verifyPreImages(recipient, 0, []);

    // Update the document before chunk-migration. This update will be coalesced to an insert
    // operation that will be cloned to the recipient. This insert operation should not get recorded
    // in the pre-image collection at the recipient shard.
    assert.commandWorked(coll.update({_id: 0}, {$set: {annotate: "update"}}));

    jsTest.log("Migrating chunk with document '{_id: 0}'");
    st.adminCommand(
        {moveChunk: coll.getFullName(), find: {_id: 0}, to: recipient.name, _waitForDelete: true});
    jsTest.log("Successfully migrated chunk with document '{_id: 0}");

    // Ensure that donor and recipient shard observed the expected 'fromMigrate' events. Note that
    // the "d" event on the donor is due to the post-migration cleanup.
    verifyFromMigrateOplogEvents(donor, 0, ["d"]);
    verifyFromMigrateOplogEvents(recipient, 0, ["i"]);

    // Ensure that the donor shard has '1' document from the update operation and recipient
    // shard has no document with '_id: 0'.
    verifyPreImages(donor, 0, ["before_update"]);
    verifyPreImages(recipient, 0, []);

    // Verify that change streams observes required events.
    verifyChangeStreamEvents(csCursor, [
        {opType: "insert", id: 0},
        {opType: "update", id: 0, prevAnnotate: "before_update", curAnnotate: "update"}
    ]);
})();

// Tests that pre-images are recorded correctly when run in-parallel with the chunk-migration.
(function testParallelUpdateDeleteAndMoveChunk() {
    // Open change streams to record all events in the collection.
    const csCursor =
        coll.watch([], {fullDocument: "required", fullDocumentBeforeChange: "required"});

    // Insert 2 documents to the collection.
    assert.commandWorked(coll.insert({_id: 1, annotate: "before_update"}));
    assert.commandWorked(coll.insert({_id: 2, annotate: "before_update"}));

    // Verify that the insert operation should not store any document to the pre-image collection.
    verifyPreImages(donor, 1, []);
    verifyPreImages(donor, 2, []);
    verifyPreImages(recipient, 1, []);
    verifyPreImages(recipient, 2, []);

    // Set the fail-point to pause the chunk migration after the clone stage.
    jsTest.log('Setting fail-point at recipient shard to pause chunk-migration after cloning.');
    pauseMigrateAtStep(recipient, migrateStepNames.cloned);

    // Spin a mongoD instance and initiate chunk-migration in parallel. The mongoD instance will
    // be used as a mode of communication.
    jsTest.log("Migration chunk with documents '{_id: 1}' and '{_id: 2}'");
    var staticMongod = MongoRunner.runMongod({});
    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {_id: 1}, null, coll.getFullName(), recipient.shardName);

    // Wait until cloning of documents is done.
    waitForMigrateStep(recipient, migrateStepNames.cloned);

    // Verify again that the no new documents are inserted at this point.
    verifyPreImages(donor, 1, []);
    verifyPreImages(donor, 2, []);
    verifyPreImages(recipient, 1, []);
    verifyPreImages(recipient, 2, []);

    // Update document with '{_id: 1}'. Update and then delete document with '{_id: 2}'. The
    // chunk-migration will transfer these to the recipient. The recipient should see them as
    // 'fromMigrate' events. The update operation to document with '{_id: 2}' should become a no-op
    // while getting transfered to the recipient shard because of the subsequent delete operation.
    // None of these events should be captured by the pre-image collection.
    assert.commandWorked(coll.update({_id: 1}, {$set: {annotate: "update"}}));
    assert.commandWorked(coll.update({_id: 2}, {$set: {annotate: "update"}}));
    assert.commandWorked(coll.deleteOne({_id: 2}));

    // Resume the chunk-migration and wait for it to complete.
    unpauseMigrateAtStep(recipient, migrateStepNames.cloned);
    joinMoveChunk();
    MongoRunner.stopMongod(staticMongod);
    jsTest.log("Successfully migrated chunk with documents '{_id: 1}' and '{_id: 2}'");

    // Verify that after the chunk-migration is complete, the pre-image collection exists on the
    // recipient shard with clustered-index enabled.
    const preImageCollInfo =
        recipient.getDB("config").getCollectionInfos({name: "system.preimages"});
    assert.eq(preImageCollInfo.length, 1, preImageCollInfo);
    assert(preImageCollInfo[0].options.hasOwnProperty("clusteredIndex"), preImageCollInfo[0]);

    // Ensure that donor and recipient shard observed the expected 'fromMigrate' events for each
    // document id. Note that the "d" event for doc 1 on the donor is due to the post-migration
    // cleanup.
    verifyFromMigrateOplogEvents(donor, 1, ["d"]);
    verifyFromMigrateOplogEvents(recipient, 1, ["i", "u"]);
    verifyFromMigrateOplogEvents(donor, 2, []);
    verifyFromMigrateOplogEvents(recipient, 2, ["i", "d"]);

    // Update the document after chunk-migration is completed. This pre-image for this update should
    // be recorded by the recipient shard.
    assert.commandWorked(coll.update({_id: 1}, {$set: {annotate: "after_migration"}}));

    // Ensure that the donor and recipient have expected pre-image after chunk-migration.
    verifyPreImages(donor, 1, ["before_update"]);
    verifyPreImages(donor, 2, ["before_update", "update"]);
    verifyPreImages(recipient, 1, ["update"]);
    verifyPreImages(recipient, 2, []);

    // Verify the change streams events.
    verifyChangeStreamEvents(csCursor, [
        {opType: "insert", id: 1},
        {opType: "insert", id: 2},
        {opType: "update", id: 1, prevAnnotate: "before_update", curAnnotate: "update"},
        {opType: "update", id: 2, prevAnnotate: "before_update", curAnnotate: "update"},
        {opType: "delete", id: 2, prevAnnotate: "update"}
    ]);
})();

st.stop();
}());
