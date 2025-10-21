/**
 * Tests that the insert batch size for the resharding collection cloner is configurable, and that
 * the default configuration enables the cloning phase in resharding to run successfully when
 * the collection contains a large number of small documents but the WiredTiger cache is small.
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagReshardingImprovements,
 *   resource_intensive,
 *   requires_wiredtiger,
 * ]
 */

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {
        // 0.25 is the minimum value for 'wiredTigerCacheSizeGB'.
        wiredTigerCacheSizeGB: 0.25
    }
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const testDb = st.s.getDB(dbName);
const testColl = testDb.getCollection(collName);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

jsTest.log("Start inserting documents");
const numDocs = 200000;
const batchSize = 10000;
{
    let docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({_id: i});
        if (docs.length % batchSize == 0) {
            assert.commandWorked(testColl.insert(docs));
            docs = [];
        }
    }
    if (docs.length > 0) {
        assert.commandWorked(testColl.insert(docs));
    }
}
jsTest.log("Finished inserting documents");

jsTest.log("Test that resharding succeeds with default reshardingCollectionClonerBatchSizeCount");
// The resharding operation below would fail with TransactionTooLargeForCache or
// TemporarilyUnavailable if the collection cloner tries to insert 16MB of documents at once.
assert.commandWorked(st.s.adminCommand({moveCollection: ns, toShard: st.shard1.shardName}));

st.stop();
