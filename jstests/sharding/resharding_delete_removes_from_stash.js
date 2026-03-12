/**
 * Tests that during resharding, a delete operation removes a document from the conflict stash
 * collection when a document with the same _id exists in both the stash and output collections.
 *
 * Scenario:
 * 1. Create a sharded collection with documents on two donors
 * 2. Start resharding to a single recipient
 * 3. After cloning begins, insert a document on donor0 with an _id that already exists
 *    in the output collection (cloned from donor1). This causes the new doc to be stashed.
 * 4. Delete this document from donor0.
 * 5. Verify Rule 1 for applying delete ops: the document is deleted from the stash collection, while the document
 *    in the output collection (from donor1) remains unchanged.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_83,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 1, reshardInPlace: false});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const kDbName = "reshardingDb";
const kCollName = "coll";
const ns = kDbName + "." + kCollName;

const inputCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

// Insert initial documents:
// - Document with _id: "conflict" on donor1 (oldKey: 10, which is >= 0)
// - Document with _id: "other" on donor0 (oldKey: -10, which is < 0)
assert.commandWorked(
    inputCollection.insert([
        {_id: "conflict", oldKey: 10, newKey: 10, data: "from_donor1"},
        {_id: "other", oldKey: -10, newKey: -10, data: "from_donor0"},
    ]),
);

jsTest.log("Initial documents inserted. Starting resharding...");

const mongos = inputCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

// Configure failpoint to pause during cloning. When this failpoint is hit, cloning is in
// progress and documents from both donors should have been or will be cloned to the output
// collection. Any writes after the clone timestamp will be applied during oplog application.
const pauseDuringCloningFp = configureFailPoint(recipient, "reshardingPauseRecipientDuringCloning");

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        // Wait for the recipient to start cloning and hit the failpoint.
        jsTest.log("Waiting for recipient to pause during cloning.");
        pauseDuringCloningFp.wait();
        jsTest.log("Recipient is paused during cloning.");

        // Now insert a document on donor0 with the same _id as an existing document
        // that will be cloned from donor1. Since the output collection will have a document
        // with _id: "conflict" after cloning completes, this new document will be placed
        // in the stash collection when the insert oplog entry is applied.
        //
        // Note: This insert happens after the clone timestamp has been established, so it
        // will be applied during the oplog application phase.
        jsTest.log("Inserting document with conflicting _id on donor0 (will go to stash collection)...");
        assert.commandWorked(
            inputCollection.insert({
                _id: "conflict",
                oldKey: -5,
                newKey: -5,
                data: "from_donor0_stashed",
            }),
        );

        // The delete should:
        // - Remove the document from the stash collection (Rule 1)
        // Both the insert and delete oplog entries will be applied in order during the
        // oplog application phase. The insert will first add the doc to the stash, then
        // the delete will remove it from the stash per Rule 1.
        jsTest.log("Deleting the conflicting document from donor0.");
        assert.commandWorked(inputCollection.deleteOne({_id: "conflict", oldKey: -5}));

        // Allow cloning to complete and oplog application to proceed.
        // The oplog entries (insert, then delete) will be applied in order.
        jsTest.log("Turning off cloning failpoint to allow resharding to complete.");
        pauseDuringCloningFp.off();
    },
);

// After resharding completes, verify the final state:
// - The document {_id: "conflict", oldKey: 10, newKey: 10} from donor1 should exist
// - The document {_id: "other", oldKey: -10, newKey: -10} from donor0 should exist
// - The stashed document from donor0 should have been deleted and not appear

jsTest.log("Resharding complete. Verifying final collection state.");

const finalDocs = inputCollection.find().sort({_id: 1}).toArray();
jsTest.log("Final documents: " + tojson(finalDocs));

assert.eq(finalDocs.length, 2, "Expected 2 documents after resharding");

// Find the document with _id: "conflict" - it should be the one from donor1
const conflictDoc = inputCollection.findOne({_id: "conflict"});
assert.neq(conflictDoc, null, "Document with _id: 'conflict' should exist");
assert.eq(
    conflictDoc.data,
    "from_donor1",
    "The surviving document should be from donor1, not the stashed one from donor0",
);
assert.eq(conflictDoc.oldKey, 10, "Document should have oldKey: 10 (from donor1)");

// Verify the other document is intact
const otherDoc = inputCollection.findOne({_id: "other"});
assert.neq(otherDoc, null, "Document with _id: 'other' should exist");
assert.eq(otherDoc.data, "from_donor0", "Document should be from donor0");

reshardingTest.teardown();
