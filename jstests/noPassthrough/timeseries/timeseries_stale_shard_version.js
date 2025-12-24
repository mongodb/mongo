/**
 * Ensure the server does not hang when a stale shard version is encountered
 * during unordered time-series group commit.
 *
 * @tags: [requires_timeseries, requires_sharding, uses_parallel_shell, requires_fcv_80]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {assertCommandWorkedInParallelShell} from "jstests/libs/parallel_shell_helpers.js";

const dbName = "timeseries_stale_shard_version";
const collName = "ts";
jsTest.log("Setting up sharded time-series collection");
const st = new ShardingTest({shards: 2, config: 1});
const db = st.s.getDB(dbName);
assert.commandWorked(
    db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(db.adminCommand({
    shardCollection: `${dbName}.${collName}`,
    timeseries: {timeField: "t", metaField: "m"},
    key: {m: 1}
}));

jsTest.log("Seeding initial bucket on shard0");
assert.commandWorked(db[collName].insertOne({t: ISODate(), m: 123}));
assert.commandWorked(db[collName].insertOne({t: ISODate(), m: 321}));

jsTest.log("Pausing two concurrent inserts before commit");
const fp = configureFailPoint(st.rs0.getPrimary(), "hangTimeseriesInsertBeforeCommit");
const awaitCreate1 = assertCommandWorkedInParallelShell(st.s, db, {
    insert: collName,
    documents: [
        {t: ISODate(), m: 222},
        {t: ISODate(), m: 333},
    ],
    ordered: false,
});
const awaitCreate2 = assertCommandWorkedInParallelShell(st.s, db, {
    insert: collName,
    documents: [
        {t: ISODate(), m: 222},
        {t: ISODate(), m: 333},
    ],
    ordered: false,
});
fp.wait();
fp.wait();

jsTest.log("Moving chunk while inserts are paused to trigger stale version on commit");
assert.commandWorked(db.adminCommand({
    moveChunk: `${dbName}.system.buckets.${collName}`,
    find: {meta: 123},
    to: st.shard1.shardName
}));

jsTest.log("Releasing inserts. The test should complete without hanging.");
fp.off();
awaitCreate1();
awaitCreate2();
jsTest.log("Both inserts completed");

st.stop();
