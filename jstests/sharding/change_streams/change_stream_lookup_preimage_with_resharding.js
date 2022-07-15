/**
 * Tests that pre-images are not recorded for the temporal resharding collection even if it has
 * pre-image recording enabled and verifies that pre-and-post-images are available for all change
 * stream events - ones that happened before, during, and after resharding of the collection.
 *
 * @tags: [
 *   requires_fcv_60,
 *   uses_change_streams,
 *   assumes_unsharded_collection,
 *   assumes_read_preference_unchanged,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

// Create a resharding test instance and enable higher frequency no-ops to avoid test case from
// failing because of timeout while waiting for next change stream event.
const reshardingTest = new ReshardingTest(
    {reshardInPlace: false, periodicNoopIntervalSecs: 1, writePeriodicNoops: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const collectionName = "test.whileResharding";

// Create a sharded collection with 'oldShardKey' as the shard key.
const coll = reshardingTest.createShardedCollection({
    ns: collectionName,
    shardKeyPattern: {oldShardKey: 1},
    chunks: [
        {min: {oldShardKey: MinKey}, max: {oldShardKey: MaxKey}, shard: donorShardNames[0]},
    ],
});

const mongos = coll.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const donorConn = new Mongo(topology.shards[donorShardNames[0]].primary);
const recipientConn = new Mongo(topology.shards[recipientShardNames[0]].primary);

// Verifies that expected documents are present in the pre-image collection at the specified shard.
function verifyPreImages(conn, expectedPreImageDocuments) {
    const preImageDocuments =
        conn.getDB("config").getCollection("system.preimages").find().toArray();

    assert.eq(preImageDocuments.length, expectedPreImageDocuments.length, preImageDocuments);

    for (let idx = 0; idx < preImageDocuments.length; idx++) {
        assert.eq(preImageDocuments[idx].preImage,
                  expectedPreImageDocuments[idx],
                  preImageDocuments[idx].preImage);
    }
}

// Verifies that the change streams cursor 'csCursor' observes the expected events.
function verifyChangeStreamEvents(csCursor, events) {
    events.forEach(expectedEvent => {
        assert.soon(() => csCursor.hasNext());
        const event = csCursor.next();

        assert.eq(event.operationType, expectedEvent.opType, event);
        assert.eq(event.documentKey._id, expectedEvent.id, event);
        assert.eq(event.fullDocumentBeforeChange.annotation, expectedEvent.prevAnnotation, event);

        if (event.operationType == "update") {
            assert.eq(event.fullDocument.annotation, expectedEvent.curAnnotation, event);
        }
    });
}

// Enable recording of pre-images in the collection.
assert.commandWorked(coll.getDB().runCommand(
    {collMod: "whileResharding", changeStreamPreAndPostImages: {enabled: true}}));

// Insert some documents before resharding the collection so that there is data to clone.
assert.commandWorked(coll.insert([
    {_id: 0, annotation: "pre-resharding-insert", oldShardKey: 0, newShardKey: 2},
    {_id: 1, annotation: "pre-resharding-insert", oldShardKey: 1, newShardKey: 3},
    {_id: 2, annotation: "pre-resharding-txn", oldShardKey: 1, newShardKey: 3},
    {_id: 3, annotation: "pre-resharding-txn", oldShardKey: 1, newShardKey: 3},
]));

// Verify that 'insert' operations does not record any pre-images.
verifyPreImages(donorConn, []);
verifyPreImages(recipientConn, []);

// Open a change stream to record all events in the collection.
const csCursor = coll.watch([], {fullDocument: "required", fullDocumentBeforeChange: "required"});

// Update documents to ensure that pre-images for these documents are recorded.
assert.commandWorked(coll.update({_id: 0}, {$set: {annotation: "pre-resharding-update"}}));
assert.commandWorked(coll.update({_id: 1}, {$set: {annotation: "pre-resharding-update"}}));

// Verify that pre-images are recorded for 'update' operations on the donor shard.
verifyPreImages(donorConn, [
    {_id: 0, annotation: "pre-resharding-insert", oldShardKey: 0, newShardKey: 2},
    {_id: 1, annotation: "pre-resharding-insert", oldShardKey: 1, newShardKey: 3}
]);
verifyPreImages(recipientConn, []);

// Reshard the collection with 'newShardKey' as the new shard key.
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newShardKey: 1},
        newChunks: [
            {min: {newShardKey: MinKey}, max: {newShardKey: MaxKey}, shard: recipientShardNames[0]}
        ],
    },
    () => {
        // We wait until cloneTimestamp has been chosen to guarantee that any subsequent writes will
        // be applied by the ReshardingOplogApplier.
        reshardingTest.awaitCloneTimestampChosen();

        assert.commandWorked(
            coll.update({_id: 0}, {$set: {annotation: "during-resharding-update"}}));
        assert.commandWorked(
            coll.update({_id: 1}, {$set: {annotation: "during-resharding-update"}}));
        assert.commandWorked(coll.remove({_id: 1}, {justOne: true}));

        // Perform some operations in a transaction.
        assert.retryNoExcept(
            () => {
                const session = coll.getDB().getMongo().startSession();
                const sessionDB = session.getDatabase(coll.getDB().getName());
                const sessionColl = sessionDB.getCollection(coll.getName());
                session.startTransaction();
                assert.commandWorked(sessionColl.update(
                    {_id: 2}, {$set: {annotation: "during-resharding-txn-update"}}));
                assert.commandWorked(sessionColl.remove({_id: 3}, {justOne: true}));
                session.commitTransaction_forTesting();
                return true;
            },
            "Failed to execute a transaction while resharding was in progress",
            10 /*num_attempts*/,
            100 /*intervalMS*/);
    });

// Verify that after the resharding is complete, the pre-image collection exists on the recipient
// shard with clustered-index enabled.
const preImageCollInfo =
    recipientConn.getDB("config").getCollectionInfos({name: "system.preimages"});
assert.eq(preImageCollInfo.length, 1, preImageCollInfo);
assert(preImageCollInfo[0].options.hasOwnProperty("clusteredIndex"), preImageCollInfo[0]);

// Update a document after resharding. The pre-image corresponding to this update should be observed
// by the recipient shard.
assert.commandWorked(coll.update({_id: 0}, {$set: {annotation: "post-resharding-update"}}));

// Verify that the donor shard contains pre-images for the update and delete operations performed
// before and during resharding and recipient shard contains pre-images from the update operation
// performed after the resharding is complete.
verifyPreImages(donorConn, [
    {_id: 0, annotation: "pre-resharding-insert", oldShardKey: 0, newShardKey: 2},
    {_id: 1, annotation: "pre-resharding-insert", oldShardKey: 1, newShardKey: 3},
    {_id: 0, annotation: "pre-resharding-update", oldShardKey: 0, newShardKey: 2},
    {_id: 1, annotation: "pre-resharding-update", oldShardKey: 1, newShardKey: 3},
    {_id: 1, annotation: "during-resharding-update", oldShardKey: 1, newShardKey: 3},
    {_id: 2, annotation: "pre-resharding-txn", oldShardKey: 1, newShardKey: 3},
    {_id: 3, annotation: "pre-resharding-txn", oldShardKey: 1, newShardKey: 3},
]);
verifyPreImages(recipientConn,
                [{_id: 0, annotation: "during-resharding-update", oldShardKey: 0, newShardKey: 2}]);

// Verify that the change stream observes the change events with required pre-images.
verifyChangeStreamEvents(csCursor, [
    {
        opType: "update",
        id: 0,
        prevAnnotation: "pre-resharding-insert",
        curAnnotation: "pre-resharding-update"
    },
    {
        opType: "update",
        id: 1,
        prevAnnotation: "pre-resharding-insert",
        curAnnotation: "pre-resharding-update"
    },
    {
        opType: "update",
        id: 0,
        prevAnnotation: "pre-resharding-update",
        curAnnotation: "during-resharding-update"
    },
    {
        opType: "update",
        id: 1,
        prevAnnotation: "pre-resharding-update",
        curAnnotation: "during-resharding-update"
    },
    {opType: "delete", id: 1, prevAnnotation: "during-resharding-update"},
    {
        opType: "update",
        id: 2,
        prevAnnotation: "pre-resharding-txn",
        curAnnotation: "during-resharding-txn-update"
    },
    {opType: "delete", id: 3, prevAnnotation: "pre-resharding-txn"},
    {
        opType: "update",
        id: 0,
        prevAnnotation: "during-resharding-update",
        curAnnotation: "post-resharding-update"
    },
]);

reshardingTest.teardown();
})();
