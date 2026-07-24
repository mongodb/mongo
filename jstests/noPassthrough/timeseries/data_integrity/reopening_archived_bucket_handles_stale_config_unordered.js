/**
 * Assert that for a sharded time-series collection, if an ordered insert fails and falls back to
 * inserting measurements unordered one at a time, and one of these inserts reopens an archived
 * bucket but encounters a StaleConfig exception while fetching the bucket document, the exception
 * does not bubble up and fail the entire insert. If it did, the router would retry the entire
 * insert, re-inserting the already-committed measurements and producing duplicates.
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "coll";
const timeFieldName = "time";
const metaFieldName = "tag";
const dataMeta = "m1";

const baseTime = ISODate("2026-07-16T10:00:00Z");
const earlierTime = ISODate("2026-07-16T09:00:00Z");

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 1}});
const mongos = st.s0;
const testDB = mongos.getDB(dbName);
assert.commandWorked(
    mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

const coll = testDB.getCollection(collName);
assert.commandWorked(
    mongos.adminCommand({
        shardCollection: coll.getFullName(),
        key: {[metaFieldName]: 1},
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    }),
);
const ddlNs = getTimeseriesCollForDDLOps(testDB, coll).getFullName();

const shard0Primary = st.rs0.getPrimary();

// Construct the batch of measurements to insert.
// The first `numDocs` measurements share a timeField and fit into one bucket `B1`; the measurement
// after that has an earlier time, which archives `B1` and opens a new bucket `B2`; the last
// measurement has the same timeField value as the first measurements and is too far ahead for `B2`,
// so it tries to reopen the archived `B1`.
const numDocs = 9;
const batchToInsert = [];
for (let i = 0; i < numDocs; ++i) {
    batchToInsert.push({[timeFieldName]: baseTime, [metaFieldName]: dataMeta, _id: i});
}
// Archives the first bucket we're inserting into.
batchToInsert.push({[timeFieldName]: earlierTime, [metaFieldName]: dataMeta, _id: "earlier"});
// Reopens the archived bucket.
batchToInsert.push({[timeFieldName]: baseTime, [metaFieldName]: dataMeta, _id: "reopening"});
const numDocsToInsert = batchToInsert.length;

// Fail the attempt to insert the batch atomically in order, triggering the fallback to insert
// measurements one at a time.
const atomicFp = configureFailPoint(shard0Primary, "failAtomicTimeseriesWrites");
// Pause the insert after it decides to reopen the archived bucket but before it fetches it.
const hangFp = configureFailPoint(shard0Primary, "hangTimeseriesReopenArchivedBucketBeforeFetch");

const awaitInsert = startParallelShell(
    funWithArgs(
        function (dbName, collName, batch) {
            assert.commandWorked(
                db.getSiblingDB(dbName).getCollection(collName).insertMany(batch, {ordered: true}),
            );
        },
        dbName,
        collName,
        batchToInsert,
    ),
    mongos.port,
);

// Wait until the insert is paused at the reopening fetch, then migrate the chunk to bump shard0's
// shard version.
hangFp.wait();
assert.commandWorked(
    mongos.adminCommand({
        movechunk: ddlNs,
        find: {meta: dataMeta},
        to: st.shard1.shardName,
    }),
);
hangFp.off();
awaitInsert();

atomicFp.off();

const numDocsInserted = coll.find().itcount();

const dupes = coll
    .aggregate([{$group: {_id: "$_id", n: {$sum: 1}}}, {$match: {n: {$gt: 1}}}])
    .toArray();

const validateRes = assert.commandWorked(coll.validate());
assert.eq(validateRes.valid, true, "Collection validation failed", {validateRes});

assert.eq(
    numDocsInserted,
    numDocsToInsert,
    "Inserted a different number of measurements than expected",
);
assert.eq(dupes.length, 0, "Inserted duplicate measurements.", {dupes});

st.stop();
