/**
 * Tests that when multiple concurrent stale routing requests target a former primary shard after
 * movePrimary, the router coalesces all stale requests into a single CSRS routing refresh, rather than
 * triggering a "convoy" of independent refreshes.
 *
 * @tags: [
 *     featureFlagShardAuthoritativeDbMetadataCRUD,
 *     featureFlagShardAuthoritativeDbMetadataDDL,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {Thread} from "jstests/libs/parallelTester.js";

const dbName = "testDB";
const collName = "testColl";
const kNumConcurrentOps = 5;

// We need two mongos instances: one for running the DDLs and one that will remain stale.
const st = new ShardingTest({
    shards: 2,
    mongos: 2,
});
const mongos0 = st.s0;
const mongos1 = st.s1;

// Create the database with shard0 as its primary shard.
assert.commandWorked(mongos0.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Create an unsharded collection with some data.
assert.commandWorked(mongos0.getDB(dbName)[collName].insertMany([{_id: 0}, {_id: 1}, {_id: 2}]));

// Prime BOTH mongos instances' routing caches so they know shard0 is the primary (V1).
assert.eq(3, mongos0.getDB(dbName)[collName].find().itcount(), "Failed to prime mongos0 routing cache");
assert.eq(3, mongos1.getDB(dbName)[collName].find().itcount(), "Failed to prime mongos1 routing cache");

// We now perform the movePrimary to make mongos1 stale.
assert.commandWorked(mongos0.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

// Enable profiling on ALL config server nodes to count how many times mongos1 fetches
// database routing metadata from the CSRS. Each fetch is a "find" on config.databases.
// We capture across all nodes because the catalog client may use Nearest read preference.
const profilingStart = new ISODate();
for (const node of st.configRS.nodes) {
    assert.commandWorked(node.getDB("config").setProfilingLevel(2, {slowms: -1}));
}

// At this point mongos will perform N concurrent requests that each results in a stale db version error.
// These should all coalesce into a single refresh.

// Setup the failpoint such that all routers pause just before processing the exception. This will let us simulate the convoying.
const allowedProgressConns = {conns: []};
const waitFb = configureFailPoint(mongos1, "waitForDBVersionCacheInvalidation", allowedProgressConns);
const threads = [];
for (let i = 0; i < kNumConcurrentOps; i++) {
    threads.push(
        new Thread(
            function (host, dbName, collName) {
                const conn = new Mongo(host);
                return conn.getDB(dbName).runCommand({find: collName, filter: {}});
            },
            mongos1.host,
            dbName,
            collName,
        ),
    );
}
for (const t of threads) t.start();

waitFb.wait({timesEntered: kNumConcurrentOps});

// Now we proceed to let each operation continue one-by-one such that each exception handling occurs after the last one has completed.
const blockedConns = mongos1
    .getDB("admin")
    .aggregate([{$currentOp: {localOps: true}}, {$project: {connectionId: 1}}])
    .toArray();

for (const conn of blockedConns) {
    if (!conn.connectionId) continue;
    allowedProgressConns.conns.push(conn.connectionId);
    configureFailPoint(mongos1, "waitForDBVersionCacheInvalidation", allowedProgressConns);
    sleep(100);
}

waitFb.off();

for (const t of threads) {
    t.join();
    const res = t.returnData();
    assert.commandWorked(res, "find should succeed after routing refresh");
}

// Count how many times the CSRS was queried for the database routing metadata.
// Each query appears as a `find` on config.databases with filter {_id: dbName}.
// We sum across all config server nodes to handle Nearest read preference.
const profileFilter = {
    ts: {$gte: profilingStart},
    "command.filter._id": dbName,
};
let totalCsrsDbQueries = 0;
const queryDetails = [];
for (const node of st.configRS.nodes) {
    const entries = node.getDB("config").system.profile.find(profileFilter).toArray();
    totalCsrsDbQueries += entries.length;
    for (const e of entries) {
        queryDetails.push(e);
    }
}

assert.eq(totalCsrsDbQueries, 1);

st.stop();
