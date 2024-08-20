/**
 * Tests that the analyzeShardKey command supports analyzing the characteristics of the shard
 * key and/or the read and write distribution.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";

const numNodesPerRS = 2;
// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

function runTest(conn) {
    const dbName = "testDb";
    const collName = "testColl";
    const numDocs = 10000;
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);

    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({x: i});
    }
    assert.commandWorked(coll.insert(docs, {writeConcern}));
    assert.commandWorked(coll.createIndex({x: 1}));

    const res0 = assert.commandWorked(conn.adminCommand({analyzeShardKey: ns, key: {x: 1}}));
    AnalyzeShardKeyUtil.assertContainKeyCharacteristicsMetrics(res0);
    AnalyzeShardKeyUtil.assertContainReadWriteDistributionMetrics(res0);

    const res1 = assert.commandWorked(
        conn.adminCommand({analyzeShardKey: ns, key: {x: 1}, keyCharacteristics: false}));
    AnalyzeShardKeyUtil.assertNotContainKeyCharacteristicsMetrics(res1);
    AnalyzeShardKeyUtil.assertContainReadWriteDistributionMetrics(res1);

    const res2 = assert.commandWorked(
        conn.adminCommand({analyzeShardKey: ns, key: {x: 1}, readWriteDistribution: false}));
    AnalyzeShardKeyUtil.assertContainKeyCharacteristicsMetrics(res2);
    AnalyzeShardKeyUtil.assertNotContainReadWriteDistributionMetrics(res2);

    const res3 = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: {x: 1}, keyCharacteristics: true, readWriteDistribution: true}));
    AnalyzeShardKeyUtil.assertContainKeyCharacteristicsMetrics(res3);
    AnalyzeShardKeyUtil.assertContainReadWriteDistributionMetrics(res3);

    // Verify that when both 'keyCharacteristics' and 'readWriteDistribution' are false,
    // the command fails because there are metrics to return.
    assert.commandFailedWithCode(conn.adminCommand({
        analyzeShardKey: ns,
        key: {x: 1},
        keyCharacteristics: false,
        readWriteDistribution: false
    }),
                                 ErrorCodes.InvalidOptions);

    // Verify that when 'readWriteDistribution' is false and the shard key does not have a
    // supporting index, the command fails because there are metrics to return.
    const res4 = assert.commandFailedWithCode(conn.adminCommand({
        analyzeShardKey: ns,
        key: {y: 1},
        keyCharacteristics: true,
        readWriteDistribution: false
    }),
                                              ErrorCodes.IllegalOperation);
    assert.eq(
        res4.errmsg,
        "Cannot analyze the characteristics of a shard key that does not have a supporting index");

    assert(coll.drop());
}

{
    const st = new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS}});
    runTest(st.s);
    st.stop();
}

{
    const rst = new ReplSetTest({nodes: numNodesPerRS});
    rst.startSet();
    rst.initiate();
    runTest(rst.getPrimary());
    rst.stopSet();
}