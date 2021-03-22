/**
 * Tests the basic functionality of the estimated size used for auto commit calculation.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

Random.setRandomSeed();

const ns = "db.collectionToReshard";

function generateData(length) {
    const chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";
    let result = [];
    for (let i = 0; i < length; i++) {
        result.push(chars[Random.randInt(chars.length)]);
    }
    return result.join('');
}

const smallData = generateData(1024);
const bigData = generateData(4 * 1024);

// Loose upper bound on the size of the documents minus the
// length of data fields. The actual size for the two documents minus the data
// field payload is 146 bytes for the shard having 1024 data length, but this is
// subject to change as the implementation changes. This is meant to establish an
// upper bound on the size of the shards based on the data length without dealing
// with the details of the storage format; this value is rounded to the nearest
// power of 2.
const maxShardBytesWithoutDataField = 256;

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

const insertedDocs = [
    {_id: "stays on shard0", oldKey: -10, newKey: -10, data: smallData},
    {_id: "moves to shard0", oldKey: 10, newKey: -10, data: bigData},
    {_id: "moves to shard1", oldKey: -10, newKey: 10, data: ""},
    {_id: "stays on shard1", oldKey: 10, newKey: 10, data: ""},
];
const numDocumentsPerShard = insertedDocs.length / 2;

assert.commandWorked(inputCollection.insert(insertedDocs));

jsTest.log('Checking estimated size reported by donors.');
const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    (tempNs) => {
        const mongos = inputCollection.getMongo();

        const getShardEstimate = (doc, shardName) => {
            let donorShards = doc.donorShards;
            for (let i = 0; i < donorShards.length; i++) {
                if (donorShards[i].id === shardName) {
                    const {bytesToClone, documentsToClone} = donorShards[i].mutableState;
                    return {bytesToClone, documentsToClone};
                }
            }
            assert(false, 'could not find ' + shardName + ' in donorShards.');
        };

        let coordinatorDoc = {};
        assert.soon(() => {
            coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: inputCollection.getFullName()
            });
            return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
        });

        jsTest.log("Check size estimate on resharding coordinator document:\n" +
                   tojson(coordinatorDoc));

        const s0Estimate = getShardEstimate(coordinatorDoc, 'shard0');
        const s1Estimate = getShardEstimate(coordinatorDoc, 'shard1');

        assert.gt(s0Estimate.bytesToClone, smallData.length);
        assert.lt(s0Estimate.bytesToClone, smallData.length + maxShardBytesWithoutDataField);

        assert.gt(s1Estimate.bytesToClone, bigData.length);
        assert.lt(s1Estimate.bytesToClone, bigData.length + maxShardBytesWithoutDataField);

        assert.eq(s0Estimate.documentsToClone, numDocumentsPerShard);
        assert.eq(s1Estimate.documentsToClone, numDocumentsPerShard);

        const verifyApproximateCopySizeForRecipients = (doc, s0Estimate, s1Estimate) => {
            const {approxBytesToCopy, approxDocumentsToCopy} = doc;
            assert(approxBytesToCopy !== undefined,
                   "Unable to find 'approxBytesToCopy' in the coordinator document");
            assert(approxDocumentsToCopy !== undefined,
                   "Unable to find 'approxDocumentsToCopy' in the coordinator document");

            const numRecipients = doc.recipientShards.length;
            assert.neq(numRecipients, 0, "Unexpected number of recipients");

            const expectedApproxDocumentsToCopy =
                (s0Estimate.documentsToClone + s1Estimate.documentsToClone) / numRecipients;
            assert.eq(approxDocumentsToCopy,
                      expectedApproxDocumentsToCopy,
                      "Unexpected value for 'approxDocumentsToCopy' in the coordinator document");

            const expectedApproxBytesToCopy =
                (s0Estimate.bytesToClone + s1Estimate.bytesToClone) / numRecipients;
            assert.eq(approxBytesToCopy,
                      expectedApproxBytesToCopy,
                      "Unexpected value for 'approxBytesToCopy' in the coordinator document");
        };
        verifyApproximateCopySizeForRecipients(coordinatorDoc, s0Estimate, s1Estimate);

        return true;
    });

reshardingTest.teardown();
})();
