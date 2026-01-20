/**
 * Test that query stats are collected, and the query stats metrics are accurate on mongos and
 * the shards in a sharded cluster when the query runs on that shard. Tests against three collection
 * types:
 * - Unsharded collection
 * - Sharded collection with data on one shard
 * - Sharded collection with data on two shards
 * @tags: [requires_fcv_83]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryStatsAggCmd,
    getQueryStatsCountCmd,
    getQueryStatsDistinctCmd,
    getQueryStatsFindCmd,
    getQueryStatsServerParameters,
    verifyMetrics,
    confirmAllExpectedFieldsPresent,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("query stats sharded cluster", function () {
    let st;
    let mongos;
    let mongosDB;
    let shard0Conn;
    let shard1Conn;

    // Collection configurations.
    const collConfigs = {
        unsharded: {
            name: "unsharded_coll",
            // Data only on shard0 (primary shard).
            expectedOnShard0: 1,
            expectedOnShard1: 0,
        },
        shardedOneShard: {
            name: "sharded_one_shard_coll",
            // Sharded but all data stays on shard0.
            expectedOnShard0: 1,
            expectedOnShard1: 0,
        },
        shardedTwoShards: {
            name: "sharded_two_shards_coll",
            // Sharded with data on both shards.
            expectedOnShard0: 1,
            expectedOnShard1: 1,
        },
    };

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {nodes: 1},
            mongosOptions: getQueryStatsServerParameters(),
            rsOptions: getQueryStatsServerParameters(),
        });

        mongos = st.s;
        mongosDB = mongos.getDB("test");
        shard0Conn = st.shard0;
        shard1Conn = st.shard1;

        assert.commandWorked(
            mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.shard0.shardName}),
        );

        setupCollections();
    });

    after(function () {
        st.stop();
    });

    beforeEach(function () {
        resetQueryStatsStore(mongos, "1MB");
        resetQueryStatsStore(shard0Conn, "1MB");
        resetQueryStatsStore(shard1Conn, "1MB");
    });

    function setupCollections() {
        // Unsharded collection - lives on primary shard (shard0).
        const unshardedColl = mongosDB[collConfigs.unsharded.name];
        unshardedColl.drop();
        assert.commandWorked(
            unshardedColl.insertMany([
                {x: 1, y: 1},
                {x: 2, y: 2},
                {x: 3, y: 3},
                {x: 4, y: 4},
            ]),
        );

        // Sharded collection on one shard - all data on shard0.
        const shardedOneShardColl = mongosDB[collConfigs.shardedOneShard.name];
        shardedOneShardColl.drop();
        assert.commandWorked(
            shardedOneShardColl.insertMany([
                {x: 1, y: 1},
                {x: 2, y: 2},
                {x: 3, y: 3},
                {x: 4, y: 4},
            ]),
        );
        shardedOneShardColl.createIndex({y: 1});
        // Don't move any chunks - all data stays on primary shard.
        assert.commandWorked(mongosDB.adminCommand({shardCollection: shardedOneShardColl.getFullName(), key: {y: 1}}));

        // Sharded collection on two shards - split data between shards.
        const shardedTwoShardsColl = mongosDB[collConfigs.shardedTwoShards.name];
        shardedTwoShardsColl.drop();
        assert.commandWorked(
            shardedTwoShardsColl.insertMany([
                {x: 1, y: -2},
                {x: 2, y: -1},
                {x: 3, y: 1},
                {x: 4, y: 2},
                {x: 5, y: 3},
            ]),
        );
        shardedTwoShardsColl.createIndex({y: 1});
        assert.commandWorked(mongosDB.adminCommand({shardCollection: shardedTwoShardsColl.getFullName(), key: {y: 1}}));
        // Split at y: 0 and move positive chunk to shard1.
        assert.commandWorked(mongosDB.adminCommand({split: shardedTwoShardsColl.getFullName(), middle: {y: 0}}));
        assert.commandWorked(
            mongosDB.adminCommand({
                moveChunk: shardedTwoShardsColl.getFullName(),
                find: {y: 1},
                to: st.shard1.shardName,
            }),
        );
    }

    function assertQueryStatsEntry({conn, getStatsFn, numEntries, description, expectedKeyOnShards}) {
        const entries = getStatsFn(conn);
        assert.eq(
            entries.length,
            numEntries,
            `${description}: Expected exactly ${numEntries} query stats entries but got: ${tojson(entries)}`,
        );

        if (numEntries > 0) {
            // Run a simple verification that the metrics make sense. We validate the key on the shards.
            // Other tests validate the mongos key.
            verifyMetrics(entries);
            if (expectedKeyOnShards) {
                confirmAllExpectedFieldsPresent(expectedKeyOnShards, entries[0].key);
            }
        }
    }

    function testCommand({commandName, getStatsFn, runCommandFn, config, expectedKeyOnShards}) {
        // Run the command.
        runCommandFn(mongosDB[config.name]);

        // Check mongos - should always have an entry.
        assertQueryStatsEntry({
            conn: mongos,
            getStatsFn,
            numEntries: 1,
            description: `${commandName} on mongos`,
        });

        // Check shard0.
        assertQueryStatsEntry({
            conn: shard0Conn,
            getStatsFn,
            numEntries: config.expectedOnShard0,
            description: `${commandName} on shard0`,
            expectedKeyOnShards: expectedKeyOnShards,
        });

        // Check shard1.
        assertQueryStatsEntry({
            conn: shard1Conn,
            getStatsFn,
            numEntries: config.expectedOnShard1,
            description: `${commandName} on shard1`,
            expectedKeyOnShards: expectedKeyOnShards,
        });
    }

    // Generate tests for each collection type.
    for (const [configName, config] of Object.entries(collConfigs)) {
        describe(`on ${configName}`, function () {
            it("should report query stats for find", function () {
                const expectedKeyOnShards = {
                    queryShape: {
                        cmdNs: {db: "test", coll: config.name},
                        command: "find",
                        filter: {x: {$gt: "?number"}},
                    },
                    client: {application: {name: "MongoDB Shell"}},
                    readConcern: {level: "local", provenance: "implicitDefault"},
                    collectionType: "collection",
                };

                if (config.expectedOnShard0 && config.expectedOnShard1) {
                    expectedKeyOnShards.queryShape.singleBatch = false;
                }

                testCommand({
                    commandName: "find",
                    getStatsFn: (conn) => getQueryStatsFindCmd(conn, {collName: config.name}),
                    runCommandFn: (coll) => {
                        coll.find({x: {$gt: 0}}).toArray();
                    },
                    config,
                    expectedKeyOnShards: expectedKeyOnShards,
                });
            });

            it("should report query stats for aggregate", function () {
                const expectedKeyOnShards = {
                    queryShape: {
                        cmdNs: {db: "test", coll: config.name},
                    },
                    client: {application: {name: "MongoDB Shell"}},
                    readConcern: {level: "local", provenance: "implicitDefault"},
                    collectionType: "collection",
                    cursor: {batchSize: "?number"},
                };

                // This will be added by mongos for tracked collections.
                if (configName != "unsharded") {
                    expectedKeyOnShards.queryShape.collation = {locale: "simple"};
                }
                expectedKeyOnShards.queryShape.let = {};
                expectedKeyOnShards.queryShape.command = "aggregate";
                expectedKeyOnShards.queryShape.pipeline = [{$match: {x: {$gte: "?number"}}}];

                testCommand({
                    commandName: "aggregate",
                    getStatsFn: (conn) => getQueryStatsAggCmd(conn.getDB(mongosDB.getName()), {collName: config.name}),
                    runCommandFn: (coll) => {
                        coll.aggregate([{$match: {x: {$gte: 1}}}]).toArray();
                    },
                    config,
                    expectedKeyOnShards: expectedKeyOnShards,
                });
            });

            it("should report query stats for count", function () {
                const expectedKeyOnShards = {
                    queryShape: {
                        cmdNs: {db: "test", coll: config.name},
                        command: "count",
                        query: {x: {$lte: "?number"}},
                    },
                    client: {application: {name: "MongoDB Shell"}},
                    readConcern: {level: "local", provenance: "implicitDefault"},
                    collectionType: "collection",
                };
                testCommand({
                    commandName: "count",
                    getStatsFn: (conn) => getQueryStatsCountCmd(conn, {collName: config.name}),
                    runCommandFn: (coll) => {
                        coll.count({x: {$lte: 10}});
                    },
                    config,
                    expectedKeyOnShards: expectedKeyOnShards,
                });
            });

            it("should report query stats for distinct", function () {
                const expectedKeyOnShards = {
                    queryShape: {
                        cmdNs: {db: "test", coll: config.name},
                        command: "distinct",
                        key: "x",
                        query: {x: {$not: {$eq: "?number"}}},
                    },
                    client: {application: {name: "MongoDB Shell"}},
                    readConcern: {level: "local", provenance: "implicitDefault"},
                    collectionType: "collection",
                };
                testCommand({
                    commandName: "distinct",
                    getStatsFn: (conn) => getQueryStatsDistinctCmd(conn, {collName: config.name}),
                    runCommandFn: (coll) => {
                        coll.distinct("x", {x: {$ne: 0}});
                    },
                    config,
                    expectedKeyOnShards: expectedKeyOnShards,
                });
            });

            // Reproduces a reparse bug caught by the fuzzer.
            it("should report query stats for $planCacheStats", function () {
                const expectedKeyOnShards = {
                    queryShape: {
                        cmdNs: {db: "test", coll: config.name},
                    },
                    client: {application: {name: "MongoDB Shell"}},
                    readConcern: {level: "local", provenance: "implicitDefault"},
                    $readPreference: {"mode": "nearest"},
                    collectionType: "collection",
                    cursor: {batchSize: "?number"},
                };
                // This will be added by mongos for tracked collections.
                if (configName != "unsharded") {
                    expectedKeyOnShards.queryShape.collation = {locale: "simple"};
                }
                expectedKeyOnShards.queryShape.let = {};
                expectedKeyOnShards.queryShape.command = "aggregate";
                expectedKeyOnShards.queryShape.pipeline = [{$planCacheStats: {allHosts: true}}];

                testCommand({
                    commandName: "aggregate",
                    getStatsFn: (conn) => getQueryStatsAggCmd(conn.getDB(mongosDB.getName()), {collName: config.name}),
                    runCommandFn: (coll) => {
                        coll.aggregate([{$planCacheStats: {allHosts: true}}]).toArray();
                    },
                    config,
                    expectedKeyOnShards: expectedKeyOnShards,
                });
            });
        });
    }
});
