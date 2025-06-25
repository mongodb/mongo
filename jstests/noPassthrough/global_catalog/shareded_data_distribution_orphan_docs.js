/*
 * Test checking that the $shardedDataDistribution aggregation stage only returns info about shards
 * owning chunks or orphan ranges.
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
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

const emptyTimeseriesNss = 'test.emptyTimeseriesNss';
const timeseriesNssWithDocs = 'test.collTimeseries2';

const primaryShard = st.shard0.shardName;
const temporaryDataShard = st.shard1.shardName;
const finalDataShard = st.shard2.shardName;

function verifyShardedDataDistributionFor(nss, expectedDistribution) {
    // TODO(SERVER-101609): This `if` statement does timeseries translation and can be removed
    if (st.s.getCollection(nss).getMetadata().type === 'timeseries') {
        nss = getTimeseriesCollForDDLOps(st.s.getDB(dbName), st.s.getCollection(nss)).getFullName();
    }

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

st.adminCommand(
    {shardcollection: emptyTimeseriesNss, key: {time: 1}, timeseries: {timeField: 'time'}});
st.adminCommand(
    {shardcollection: timeseriesNssWithDocs, key: {time: 1}, timeseries: {timeField: 'time'}});

// Insert data to validate the aggregation stage
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(st.s.getCollection(nssWithDocs).insert({skey: i}));
}

for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(st.s.getCollection(timeseriesNssWithDocs)
                             .insertOne({'time': ISODate('2021-05-18T00:00:00.000Z'), 'temp': i}));
}

// Move the chunk of each collection twice, so that it visits each shard of the cluster.
for (let destinationShardId of [temporaryDataShard, finalDataShard]) {
    for (let nss of [emptyNss, nssWithDocs]) {
        assert.commandWorked(
            st.s.adminCommand({moveChunk: nss, find: {skey: 1}, to: destinationShardId}));
    }
    for (let nss of [timeseriesNssWithDocs, emptyTimeseriesNss]) {
        assert.commandWorked(st.s.adminCommand({
            moveChunk: getTimeseriesCollForDDLOps(st.s.getDB(dbName), st.s.getCollection(nss))
                           .getFullName(),
            find: {"control.min.time": 1},
            to: destinationShardId
        }));
    }
}

// The empty collection will have transitioned through the shards of the cluster without leaving
// orphans; verify that $shardedDataDistribution only returns information about the current data
// shard.
verifyShardedDataDistributionFor(
    emptyNss, [{shardName: finalDataShard, numOwnedDocuments: 0, numOrphanedDocs: 0}]);

// The non-empty collection, on the other hand, will have left orphan documents on each previous
// data shard - and all the docs should be present on the current one.
verifyShardedDataDistributionFor(nssWithDocs, [
    {shardName: primaryShard, numOwnedDocuments: 0, numOrphanedDocs: numDocs},
    {shardName: temporaryDataShard, numOwnedDocuments: 0, numOrphanedDocs: numDocs},
    {shardName: finalDataShard, numOwnedDocuments: numDocs, numOrphanedDocs: 0}
]);

verifyShardedDataDistributionFor(
    emptyTimeseriesNss, [{shardName: finalDataShard, numOwnedDocuments: 0, numOrphanedDocs: 0}]);
verifyShardedDataDistributionFor(timeseriesNssWithDocs, [
    {shardName: primaryShard, numOwnedDocuments: 0, numOrphanedDocs: 1},
    {shardName: temporaryDataShard, numOwnedDocuments: 0, numOrphanedDocs: 1},
    {shardName: finalDataShard, numOwnedDocuments: 1, numOrphanedDocs: 0}
]);

st.stop();
