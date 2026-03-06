/**
 * Tests that when a client operation times out due to MaxTimeMS, the extended deadline on mongos
 * prevents connection churn from occurring. The shard should enforce maxTimeMSOpOnly and return
 * MaxTimeMSExpired before mongos's local timer fires, so the connection is returned to the pool
 * healthy rather than being torn down.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_81,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    shardOptions: {
        setParameter: {enableTestCommands: 1},
    },
    mongosOptions: {
        setParameter: {enableTestCommands: 1, maxTimeMsLocalBufferTimeMillis: 2000},
    },
});

const mongos = st.s;
const mongosAdminDB = mongos.getDB("admin");
const testDB = mongos.getDB(jsTestName());

function getShardHostPoolStats() {
    const poolStats = assert.commandWorked(mongosAdminDB.runCommand({connPoolStats: 1}));
    let totalCreated = 0;
    let totalAvailable = 0;
    let totalInUse = 0;
    if (poolStats.hosts) {
        for (const host in poolStats.hosts) {
            if (poolStats.hosts.hasOwnProperty(host)) {
                totalCreated += poolStats.hosts[host].created || 0;
                totalAvailable += poolStats.hosts[host].available || 0;
                totalInUse += poolStats.hosts[host].inUse || 0;
            }
        }
    }
    return {totalCreated, totalAvailable, totalInUse, totalInPool: totalAvailable + totalInUse};
}

const collName = jsTestName();
const coll = testDB[collName];

// Insert documents that will be scanned by the slow $where filter.
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= 1000; i++) {
    bulk.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute());

// Warm up the connection pool by running a simple command.
assert.commandWorked(testDB.runCommand({ping: 1}));

// Wait for the connection pool to stabilize: no connections should be in-use.
assert.soon(() => {
    const stats = getShardHostPoolStats();
    return stats.totalInUse === 0 && stats.totalAvailable > 0;
}, "Timed out waiting for connection pool to stabilize after warmup");

const statsBefore = getShardHostPoolStats();
jsTestLog("Connection pool stats before timeout test: " + tojson(statsBefore));

// Use $where to cause a slow operation that will exceed maxTimeMS. The shard should enforce
// maxTimeMSOpOnly and return MaxTimeMSExpired before mongos's local timer (deadline + buffer)
// fires. This means the connection should be returned healthy, not torn down.
const maxTimeMSValue = 4000; // 4000ms - operation will timeout on the shard

jsTestLog("Running slow find command with maxTimeMS=" + maxTimeMSValue + "ms");
const findResult = testDB.runCommand({
    find: collName,
    filter: {$where: "sleep(50); return true;"}, // 50ms sleep per document
    maxTimeMS: maxTimeMSValue,
});

jsTestLog("Find command result: " + tojson(findResult));
assert.commandFailedWithCode(findResult, ErrorCodes.MaxTimeMSExpired);

// Run another command to trigger any pending connection pool operations and ensure the
// connection used above is returned to the pool.
assert.commandWorked(testDB.runCommand({ping: 1}));

// Wait for pool to settle again.
assert.soon(() => {
    return getShardHostPoolStats().totalInUse === 0;
}, "Timed out waiting for connections to be returned to pool after timeout test");

const statsAfter = getShardHostPoolStats();
jsTestLog("Connection pool stats after timeout test: " + tojson(statsAfter));

// The key assertion: no NEW connections should have been created. If the connection was torn
// down due to the timeout, a new one would have been created to replace it.
const newConnections = statsAfter.totalCreated - statsBefore.totalCreated;
jsTestLog("New connections created during timeout test: " + newConnections);
assert.eq(
    newConnections,
    0,
    "Expected no new connections to be created, but " +
        newConnections +
        " were created. This suggests the MaxTimeMSExpired error caused connection churn." +
        " Before: " +
        tojson(statsBefore) +
        " After: " +
        tojson(statsAfter),
);

// Also verify the pool size didn't shrink (connections weren't dropped).
assert.gte(
    statsAfter.totalInPool,
    statsBefore.totalInPool,
    "Connection pool size decreased after timeout, indicating connection churn." +
        " Before: " +
        tojson(statsBefore) +
        " After: " +
        tojson(statsAfter),
);

// Cleanup.
assert.commandWorked(testDB.dropDatabase());

st.stop();
