/**
 * Tests resharding oplog application is unaffected when operations from separate donors have
 * recordId collisions.
 *
 * @tags: [
 *  featureFlagRecordIdsReplicated,
 *  # Expects duplicated RecordIds from inserts across shards. Clustered collections have RecordIds
 *  # determined by '_id', which is expected to be globally unique for sharding operations to
 *  # succeed.
 *  expects_explicit_underscore_id_index,
 *  uses_atclustertime,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const reshardingTest = new ReshardingTest({numDonors: 2});

reshardingTest.setup();

const ns = `${jsTestName()}.coll`;
const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

function assertShardContents(shardConn, expectedDocs) {
    // We sort by oldKey so the order of `expectedDocs` can be deterministic.
    assert.eq(
        expectedDocs,
        shardConn.getCollection(inputCollection.getFullName()).find().sort({_id: 1}).toArray(),
        `Expected: ${tojson(expectedDocs)}, got actual ${tojson(inputCollection.find().sort({_id: 1}).toArray())}`,
    );
}

function hasDuplicateRids(docsWithRecordIds) {
    const recordIds = docsWithRecordIds.map((doc) => doc.$recordId.valueOf());
    return new Set(recordIds).size !== docsWithRecordIds.length;
}

// Docs copied during the cloning phase (inserted before the clone timestamp is chosen).
const donor0Docs = [
    {_id: "a0", note: "starts donorShard0", oldKey: -10, newKey: 0},
    {_id: "a1", note: "starts donorShard0", oldKey: -10, newKey: 0},
    {_id: "a2", note: "starts donorShard0", oldKey: -10, newKey: 0},
];
const donor1Docs = [
    {_id: "b0", note: "starts donorShard1", oldKey: 10, newKey: 0},
    {_id: "b1", note: "starts donorShard1", oldKey: 10, newKey: 0},
    {_id: "b2", note: "starts donorShard1", oldKey: 10, newKey: 0},
];

// Docs inserted during the oplog application phase of resharding. Inserted after the clone timestamp is chosen.
const donor0DocsOplogApp = [
    {_id: "aa0", note: "oplogApp. starts donorShard0", oldKey: -10, newKey: 0},
    {_id: "aa1", note: "oplogApp. starts donorShard0", oldKey: -10, newKey: 0},
    {_id: "aa2", note: "oplogApp. starts donorShard0", oldKey: -10, newKey: 0},
];
const donor1DocsOplogApp = [
    {_id: "bb0", note: "oplogApp. donorShard1", oldKey: 10, newKey: 0},
    {_id: "bb1", note: "oplogApp. starts donorShard1", oldKey: 10, newKey: 0},
    {_id: "bb2", note: "oplogApp. starts donorShard1", oldKey: 10, newKey: 0},
];

const recipientShardNames = reshardingTest.recipientShardNames;

jsTestLog(
    `Resharding toplogy -  donor0: ${donorShardNames[0]}, donor1: ${donorShardNames[1]}, recipient: ${recipientShardNames[0]}`,
);
assert.commandWorked(inputCollection.insert(donor0Docs));
assert.commandWorked(inputCollection.insert(donor1Docs));

// Check the initial donor contents are on the correct chunks.
const mongos = inputCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const donor0 = new Mongo(topology.shards[donorShardNames[0]].primary);
const donor1 = new Mongo(topology.shards[donorShardNames[1]].primary);
assertShardContents(donor0, donor0Docs);
assertShardContents(donor1, donor1Docs);

const recipient0 = new Mongo(topology.shards[recipientShardNames[0]].primary);

// Pausing before cloning but after establishing the clone timestamp ensures documents inserted are
// going to be applied during the oplog application phase later on.
const reshardingPauseRecipientBeforeCloningFailpoint = configureFailPoint(
    recipient0,
    "reshardingPauseRecipientBeforeCloning",
);

// All chunks are relocated to the recipient.
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        reshardingPauseRecipientBeforeCloningFailpoint.wait();
        jsTest.log(
            `The clone timestamp has been chosen. Inserting documents on each donor that will be applied via resharding oplog application`,
        );
        // The documents inserted here should have overlapping recordIds (RecordIds 4, 5, & 6).
        for (let i = 0; i < donor0DocsOplogApp.length; i++) {
            assert.commandWorked(inputCollection.insert(donor0DocsOplogApp[i]));
            assert.commandWorked(inputCollection.insert(donor1DocsOplogApp[i]));
        }
        const docsWithRids = inputCollection.find().showRecordId().toArray();
        assert(
            hasDuplicateRids(docsWithRids),
            `Expected the recordIds across the two donor shards to have overlapping ids. Found docs with recordIds: ${tojson(docsWithRids)}`,
        );

        jsTest.log(`Done inserting documents to be applied during recipient oplog application`);
        reshardingPauseRecipientBeforeCloningFailpoint.off();
    },
);

assertShardContents(recipient0, [...donor0Docs, ...donor0DocsOplogApp, ...donor1Docs, ...donor1DocsOplogApp]);
reshardingTest.teardown();
