/*
 * Test checking that the $shardedDataDistribution aggregation stage only returns info about shards
 * owning chunks or orphan ranges.
 */

import {ShardingTest} from 'jstests/libs/shardingtest.js';

// Skip orphans check because this test disables the range deleter in order to test the
// `numOrphanDocs` response field.
TestData.skipCheckOrphans = true;

const st = new ShardingTest({shards: 3, rs: {setParameter: {disableResumableRangeDeleter: true}}});
const mongos = st.s;

const adminDb = mongos.getDB('admin');
const dbName = 'test';
const emptyNss = 'test.emptyNss';
const nssWithDocs = 'test.coll2';
const numDocs = 6;

const primaryShard = st.shard0.shardName;
const temporaryDataShard = st.shard1.shardName;
const finalDataShard = st.shard2.shardName;

function buildExpectedDataOnShard(shardName, expectedNumDocs, expectedNumOrphans) {
    return {
        shardName: shardName,
        numOwnedDocuments: expectedNumDocs,
        numOrphanedDocs: expectedNumOrphans
    };
}

function verifyShardedDataDistributionFor(nss, expectedDistribution) {
    const aggregationResponse =
        adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: nss}}]).toArray();
    assert.eq(1, aggregationResponse.length);
    const dataDistribution = aggregationResponse[0];
    assert.eq(dataDistribution.ns, nss);
    assert.sameMembers(dataDistribution.shards,
                       expectedDistribution,
                       'Unexpected response from $shardedDataDistribution',
                       (d1, d2) => {
                           return d1.shardName === d2.shardName &&
                               d1.numOwnedDocuments === d2.numOwnedDocuments &&
                               d1.numOrphanedDocs === d2.numOrphanedDocs;
                       });
}

// Setup the namespaces to be tested.
st.adminCommand({enablesharding: dbName, primaryShard: primaryShard});
st.adminCommand({shardcollection: emptyNss, key: {skey: 1}});
st.adminCommand({shardcollection: nssWithDocs, key: {skey: 1}});

// Insert data to validate the aggregation stage
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(st.s.getCollection(nssWithDocs).insert({skey: i}));
}

// Move the chunk of each collection twice, so that it visits each shard of the cluster.
for (let nss of [emptyNss, nssWithDocs]) {
    for (let destinationShardId of [temporaryDataShard, finalDataShard]) {
        assert.commandWorked(
            st.s.adminCommand({moveChunk: nss, find: {skey: 1}, to: destinationShardId}));
    }
}

// The empty collection will have transitioned through the shards of the cluster without leaving
// orphans; verify that $shardedDataDistribution only returns information about the current data
// shard.
let expectedDistributionForEmptyNss =
    [buildExpectedDataOnShard(finalDataShard, 0 /*expectedNumDocs*/, 0 /*expectedNumOrphans*/)];

verifyShardedDataDistributionFor(emptyNss, expectedDistributionForEmptyNss);

// The non-empty collection, on the other hand, will have left orphan documents on each previous
// data shard - and all the docs should be present on the current one.
let expectedDistributionForNssWithDocs = [
    buildExpectedDataOnShard(primaryShard, 0 /*expectedNumDocs*/, numDocs /*expectedNumOrphans*/),
    buildExpectedDataOnShard(
        temporaryDataShard, 0 /*expectedNumDocs*/, numDocs /*expectedNumOrphans*/),
    buildExpectedDataOnShard(finalDataShard, numDocs /*expectedNumDocs*/, 0 /*expectedNumOrphans*/)
];

verifyShardedDataDistributionFor(nssWithDocs, expectedDistributionForNssWithDocs);

st.stop();
