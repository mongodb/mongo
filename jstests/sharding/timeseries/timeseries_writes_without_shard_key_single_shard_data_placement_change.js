/**
 * Test timeseries writes without shard key in a transaction with snapshot read concern fails during
 * data placement change.
 *
 * @tags: [
 *    featureFlagTimeseriesUpdatesSupport,
 *    requires_timeseries,
 *    requires_sharding,
 *    uses_transactions,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const collName2 = "testColl2";
const nss = dbName + "." + collName;
const splitPoint = ISODate("2024-01-02T00:00:00.00Z");
const docsToInsert = [
    {_id: 1, timestamp: ISODate("2024-01-01T00:00:00.00Z"), y: 1},
    {_id: 2, timestamp: ISODate("2024-01-03T00:00:00.00Z"), y: 2},
];

const dbConn = st.s.getDB(dbName);
const coll = dbConn.getCollection(collName);

// Sets up a 2 shard cluster using 'timestamp' as a shard key where Shard 0 owns timestamp <
// splitPoint and Shard 1 splitPoint >= splitPoint.
assert.commandWorked(dbConn.createCollection(
    collName,
    {timeseries: {timeField: "timestamp", bucketMaxSpanSeconds: 1, bucketRoundingSeconds: 1}}));

assert.commandWorked(dbConn.adminCommand({'enableSharding': dbName}));
assert.commandWorked(dbConn.adminCommand({shardCollection: nss, key: {timestamp: 1}}));

const splitNs = `${dbName}.system.buckets.${collName}`;
const splitKey = "control.min.timestamp";

assert.commandWorked(dbConn.adminCommand({split: splitNs, middle: {[splitKey]: splitPoint}}));

// Move chunks so each shard has one chunk.
assert.commandWorked(dbConn.adminCommand({
    moveChunk: splitNs,
    find: {[splitKey]: splitPoint},
    to: st.shard1.shardName,
}));

assert.commandWorked(coll.insert(docsToInsert));

function runTest(testCase) {
    const session = st.s.startSession();
    session.startTransaction({readConcern: {level: "snapshot"}});
    session.getDatabase(dbName).getCollection(collName2).insert({x: 1});
    let hangDonorAtStartOfRangeDel =
        configureFailPoint(st.rs1.getPrimary(), "suspendRangeDeletion");

    // Move all chunks for testDb.testColl to shard0.
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: splitNs, find: {[splitKey]: splitPoint}, to: st.shard0.shardName}));
    hangDonorAtStartOfRangeDel.wait();

    // This command MUST fail, the data moved to another shard, we can't try on shard0 nor
    // shard1 with the original clusterTime of the transaction.
    assert.commandFailedWithCode(session.getDatabase(dbName).runCommand(testCase.cmdObj),
                                 ErrorCodes.MigrationConflict);

    hangDonorAtStartOfRangeDel.off();

    // Reset the chunk distribution for the next test.
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: splitNs, find: {[splitKey]: splitPoint}, to: st.shard1.shardName}));
}

let testCases = [
    {
        logMessage: "Running timeseries updateOne test",
        cmdObj: {
            update: collName,
            updates:
                [{q: {timestamp: ISODate("2024-01-03T00:00:00.00Z"), y: 2}, u: {$inc: {z: 1}}}],
        },
    },
    {
        logMessage: "Running timeseries deleteOne test.",
        cmdObj: {
            delete: collName,
            deletes: [{q: {timestamp: ISODate("2024-01-03T00:00:00.00Z"), y: 2}, limit: 1}],
        },
    }
];

testCases.forEach(testCase => {
    jsTestLog(testCase.logMessage);
    runTest(testCase);
});

st.stop();
