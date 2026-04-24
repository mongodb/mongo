/**
 * Tests that 'originalQueryShapeHash' appears in the query stats key on shards when a command
 * originates from a router, and that the value matches the router's own queryShapeHash for the
 * same query. Also verifies that 'originalQueryShapeHash' is absent from the key when a command
 * is run directly against a mongod (no router involved).
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {
    getQueryStats,
    getQueryStatsFindCmd,
    getQueryStatsAggCmd,
    getQueryStatsCountCmd,
    getQueryStatsDistinctCmd,
    getQueryStatsServerParameters,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "test_coll";

const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    rs: {nodes: 1},
    mongosOptions: getQueryStatsServerParameters(),
    rsOptions: getQueryStatsServerParameters(),
});

const mongos = st.s;
const mongosDB = mongos.getDB(dbName);
const shard0 = st.shard0;
const shard0DB = shard0.getDB(dbName);

const coll = mongosDB[collName];
assert.commandWorked(
    coll.insertMany([
        {x: 1, y: 1},
        {x: 2, y: 2},
        {x: 3, y: 3},
    ]),
);

(function testFind() {
    resetQueryStatsStore(mongos, "1MB");
    resetQueryStatsStore(shard0, "1MB");

    // Run a find via the router.
    assert.eq(coll.find({x: {$gt: 0}}).itcount(), 3);

    // Shard's query stats key should include originalQueryShapeHash.
    const shardEntries = getQueryStats(shard0, {collName, extraMatch: {"key.originalQueryShapeHash": {$exists: true}}});
    assert.eq(
        shardEntries.length,
        1,
        "Expected exactly one shard entry with originalQueryShapeHash for find: " + tojson(shardEntries),
    );

    const shardEntry = shardEntries[0];
    assert.eq(shardEntry.key.queryShape.command, "find");

    // The router's queryShapeHash for this find must match the shard's originalQueryShapeHash.
    const routerEntries = getQueryStats(mongos, {collName});
    assert.gt(routerEntries.length, 0, "Expected at least one router query stats entry for find");
    const routerQueryShapeHash = routerEntries[0].queryShapeHash;
    assert.eq(
        shardEntry.key.originalQueryShapeHash,
        routerQueryShapeHash,
        "Shard originalQueryShapeHash should match router queryShapeHash for find",
    );
})();

(function testAggregate() {
    resetQueryStatsStore(mongos, "1MB");
    resetQueryStatsStore(shard0, "1MB");

    // Run an aggregate via the router.
    assert.eq(coll.aggregate([{$match: {x: {$gt: 0}}}]).toArray().length, 3);

    const shardEntries = getQueryStats(shard0, {collName, extraMatch: {"key.originalQueryShapeHash": {$exists: true}}});
    assert.eq(
        shardEntries.length,
        1,
        "Expected exactly one shard entry with originalQueryShapeHash for agg: " + tojson(shardEntries),
    );

    const shardEntry = shardEntries[0];
    assert.eq(shardEntry.key.queryShape.command, "aggregate");

    const routerEntries = getQueryStats(mongos, {collName});
    assert.gt(routerEntries.length, 0, "Expected at least one router query stats entry for agg");
    assert.eq(
        shardEntry.key.originalQueryShapeHash,
        routerEntries[0].queryShapeHash,
        "Shard originalQueryShapeHash should match router queryShapeHash for aggregate",
    );
})();

(function testCount() {
    resetQueryStatsStore(mongos, "1MB");
    resetQueryStatsStore(shard0, "1MB");

    // Run a count via the router.
    const countResult = assert.commandWorked(mongosDB.runCommand({count: collName, query: {x: {$gt: 0}}}));
    assert.eq(countResult.n, 3);

    const shardEntries = getQueryStats(shard0, {collName, extraMatch: {"key.originalQueryShapeHash": {$exists: true}}});
    assert.eq(
        shardEntries.length,
        1,
        "Expected exactly one shard entry with originalQueryShapeHash for count: " + tojson(shardEntries),
    );

    const shardEntry = shardEntries[0];
    assert.eq(shardEntry.key.queryShape.command, "count");

    const routerEntries = getQueryStats(mongos, {collName});
    assert.gt(routerEntries.length, 0, "Expected at least one router query stats entry for count");
    assert.eq(
        shardEntry.key.originalQueryShapeHash,
        routerEntries[0].queryShapeHash,
        "Shard originalQueryShapeHash should match router queryShapeHash for count",
    );
})();

(function testDistinct() {
    resetQueryStatsStore(mongos, "1MB");
    resetQueryStatsStore(shard0, "1MB");

    // Run a distinct via the router.
    assert.commandWorked(mongosDB.runCommand({distinct: collName, key: "x", query: {x: {$gt: 0}}}));

    const shardEntries = getQueryStats(shard0, {collName, extraMatch: {"key.originalQueryShapeHash": {$exists: true}}});
    assert.eq(
        shardEntries.length,
        1,
        "Expected exactly one shard entry with originalQueryShapeHash for distinct: " + tojson(shardEntries),
    );

    const shardEntry = shardEntries[0];
    assert.eq(shardEntry.key.queryShape.command, "distinct");

    const routerEntries = getQueryStats(mongos, {collName});
    assert.gt(routerEntries.length, 0, "Expected at least one router query stats entry for distinct");
    assert.eq(
        shardEntry.key.originalQueryShapeHash,
        routerEntries[0].queryShapeHash,
        "Shard originalQueryShapeHash should match router queryShapeHash for distinct",
    );
})();

(function testDirectToShardHasNoOriginalQueryShapeHash() {
    resetQueryStatsStore(shard0, "1MB");

    // Run directly against the shard (no router, so no originalQueryShapeHash set).
    assert.eq(shard0DB[collName].find({x: {$gt: 0}}).itcount(), 3);

    const allEntries = getQueryStats(shard0, {collName});
    assert.gt(allEntries.length, 0, "Expected at least one shard query stats entry");

    for (const entry of allEntries) {
        assert(
            !entry.key.hasOwnProperty("originalQueryShapeHash"),
            "Direct-to-shard query should not have originalQueryShapeHash in key: " + tojson(entry),
        );
    }
})();

st.stop();
