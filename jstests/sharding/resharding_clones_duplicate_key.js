/**
 * Tests that unrecoverable errors during resharding's collection cloning are handled by the
 * recipient.
 *
 * @tags: [
 *   uses_atclustertime
 * ]
 */
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 1});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

// The following documents violate the global _id uniqueness assumption of sharded collections. It
// is possible to construct such a sharded collection due to how each shard independently enforces
// the uniqueness of _id values for only the documents it owns. The resharding operation is expected
// to abort upon discovering this violation.
assert.commandWorked(
    inputCollection.insert([
        {_id: 0, info: `moves from ${donorShardNames[0]}`, oldKey: -10, newKey: -10},
        {_id: 0, info: `moves from ${donorShardNames[1]}`, oldKey: 10, newKey: 10},
    ]),
);

// The collection is cloned in ascending _id order so we insert some large documents with higher _id
// values to guarantee there will be a cursor needing to be cleaned up on the donor shards after
// cloning errors.
const largeStr = "x".repeat(9 * 1024 * 1024);
assert.commandWorked(
    inputCollection.insert([
        {_id: 10, info: `moves from ${donorShardNames[0]}`, oldKey: -10, newKey: -10, pad: largeStr},
        {_id: 11, info: `moves from ${donorShardNames[0]}`, oldKey: -10, newKey: -10, pad: largeStr},
        {_id: 20, info: `moves from ${donorShardNames[1]}`, oldKey: 10, newKey: 10, pad: largeStr},
        {_id: 21, info: `moves from ${donorShardNames[1]}`, oldKey: 10, newKey: 10, pad: largeStr},
    ]),
);

const mongos = inputCollection.getMongo();
const recipientShardNames = reshardingTest.recipientShardNames;

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {},
    {expectedErrorCode: ErrorCodes.DuplicateKey},
);

const timeout = 60 * 1000;
assert.soon(
    () => {
        const idleCursors = mongos
            .getDB("admin")
            .aggregate([
                {$currentOp: {allUsers: true, idleCursors: true}},
                {$match: {type: "idleCursor", ns: inputCollection.getFullName()}},
            ])
            .toArray();
        if (idleCursors.length > 0) {
            jsTest.log("Found idle cursors: " + tojson(idleCursors));
        }
        return idleCursors.length == 0;
    },
    "timed out awaiting cloning cursors to be cleaned up",
    timeout,
);

reshardingTest.teardown();
