/**
 * Test that peakTrackedMemBytes and clusterPeakTrackedMemBytes are included in $queryStats output for both standalone, sharded, and when mongod acts as a router.
 */
import {describe, it, before, beforeEach, after} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getQueryStatsFindCmd, getQueryStatsAggCmd, getQueryExecMetrics} from "jstests/libs/query/query_stats_utils.js";

describe("peakTrackedMemBytes in queryStats - standalone", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalQueryStatsRateLimit: -1,
                internalQueryMaxWriteToCurOpMemoryUsageBytes: 0,
            },
        });
        this.testDB = this.conn.getDB("test");
        this.coll = this.testDB["peak_mem_standalone"];
        this.coll.drop();
        const bulk = this.coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 100; i++) {
            bulk.insert({a: i, b: "padding_" + "x".repeat(100)});
        }
        assert.commandWorked(bulk.execute());
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("report peakTrackedMemBytes & clusterPeakTrackedMemBytes", function () {
        this.coll.find({}).sort({a: -1}).toArray();

        const queryStatsResults = getQueryStatsFindCmd(this.conn, {collName: this.coll.getName()});
        assert.eq(queryStatsResults.length, 1, queryStatsResults);

        const queryExecMetrics = getQueryExecMetrics(queryStatsResults[0].metrics);
        assert.gt(queryExecMetrics.peakTrackedMemBytes.sum, 0);
        assert.eq(queryExecMetrics.clusterPeakTrackedMemBytes.sum, queryExecMetrics.peakTrackedMemBytes.sum);

        // Clear the cache so the getMore query gets its own fresh entry.
        assert.commandWorked(this.conn.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "0MB"}));
        assert.commandWorked(this.conn.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "1MB"}));

        // Use a small batchSize to force multiple getMore calls with an in-memory sort.
        this.coll.find({}).sort({a: -1}).batchSize(5).toArray();

        const getMoreQueryStatsResults = getQueryStatsFindCmd(this.conn, {collName: this.coll.getName()});
        assert.eq(getMoreQueryStatsResults.length, 1, getMoreQueryStatsResults);

        const getMoreQueryExecMetrics = getQueryExecMetrics(getMoreQueryStatsResults[0].metrics);
        assert.gt(getMoreQueryExecMetrics.peakTrackedMemBytes.sum, 0);
        assert.eq(
            getMoreQueryExecMetrics.clusterPeakTrackedMemBytes.sum,
            getMoreQueryExecMetrics.peakTrackedMemBytes.sum,
        );

        // Check that getMore's peakTracked is not unreasonably higher than initial peakTracked metric to confirm we aren't double-counting.
        assert.lt(getMoreQueryExecMetrics.peakTrackedMemBytes.sum, 2 * queryExecMetrics.peakTrackedMemBytes.sum);
    });
});

describe("peakTrackedMemBytes in queryStats - sharded cluster", function () {
    before(function () {
        const queryStatsParams = {
            setParameter: {
                internalQueryStatsRateLimit: -1,
                internalQueryMaxWriteToCurOpMemoryUsageBytes: 0,
            },
        };
        this.st = new ShardingTest({
            shards: 3,
            mongos: 1,
            rs: {nodes: 1},
            mongosOptions: queryStatsParams,
            rsOptions: queryStatsParams,
        });
        this.mongos = this.st.s;
        this.mongosDB = this.mongos.getDB("test");
        this.shard0 = this.st.shard0;
        this.shard1 = this.st.shard1;
        this.shard2 = this.st.shard2;

        const collName = "peak_mem_sharded";
        this.coll = this.mongosDB[collName];
        this.coll.drop();

        assert.commandWorked(
            this.mongosDB.adminCommand({enableSharding: "test", primaryShard: this.st.shard0.shardName}),
        );

        //Shard and move documents to all three shards.
        assert.commandWorked(this.mongosDB.adminCommand({shardCollection: this.coll.getFullName(), key: {a: 1}}));
        assert.commandWorked(this.mongosDB.adminCommand({split: this.coll.getFullName(), middle: {a: 67}}));
        assert.commandWorked(this.mongosDB.adminCommand({split: this.coll.getFullName(), middle: {a: 134}}));
        assert.commandWorked(
            this.mongosDB.adminCommand({
                moveChunk: this.coll.getFullName(),
                find: {a: 67},
                to: this.st.shard1.shardName,
            }),
        );
        assert.commandWorked(
            this.mongosDB.adminCommand({
                moveChunk: this.coll.getFullName(),
                find: {a: 134},
                to: this.st.shard2.shardName,
            }),
        );

        const bulk = this.coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 200; i++) {
            bulk.insert({a: i, b: "padding_" + "x".repeat(100)});
        }
        assert.commandWorked(bulk.execute());
    });

    after(function () {
        this.st.stop();
    });

    it("mongos reports both peakTrackedMemBytes and clusterPeakTrackedMemBytes", function () {
        this.coll
            .aggregate([
                {$group: {_id: {aModulo: {$mod: ["$a", 10]}}, total: {$sum: 1}, data: {$push: "$b"}}},
                {$sort: {_id: 1}},
            ])
            .toArray();

        const mongosStats = getQueryStatsAggCmd(this.mongosDB, {collName: this.coll.getName()});
        assert.eq(mongosStats.length, 1, tojson(mongosStats));
        const mongosMetrics = getQueryExecMetrics(mongosStats[0].metrics);
        const mongosLocalPeak = mongosMetrics.peakTrackedMemBytes.sum;
        const mongosTotalPeak = mongosMetrics.clusterPeakTrackedMemBytes.sum;

        const shard0Stats = getQueryStatsAggCmd(this.shard0.getDB("test"), {collName: this.coll.getName()});
        const shard1Stats = getQueryStatsAggCmd(this.shard1.getDB("test"), {collName: this.coll.getName()});
        const shard2Stats = getQueryStatsAggCmd(this.shard2.getDB("test"), {collName: this.coll.getName()});

        const shard0Exec = getQueryExecMetrics(shard0Stats[0].metrics);
        const shard1Exec = getQueryExecMetrics(shard1Stats[0].metrics);
        const shard2Exec = getQueryExecMetrics(shard2Stats[0].metrics);

        const shard0Peak = shard0Exec.peakTrackedMemBytes.sum;
        const shard1Peak = shard1Exec.peakTrackedMemBytes.sum;
        const shard2Peak = shard2Exec.peakTrackedMemBytes.sum;

        const shard0TotalPeak = shard0Exec.clusterPeakTrackedMemBytes.sum;
        const shard1TotalPeak = shard1Exec.clusterPeakTrackedMemBytes.sum;
        const shard2TotalPeak = shard2Exec.clusterPeakTrackedMemBytes.sum;

        assert.gt(mongosTotalPeak, mongosLocalPeak);
        assert.eq(shard0TotalPeak, shard0Peak);
        assert.eq(shard1TotalPeak, shard1Peak);
        assert.eq(shard2TotalPeak, shard2Peak);

        // Check that overall metrics make sense.
        const totalShardPeak = shard0TotalPeak + shard1TotalPeak + shard2TotalPeak;
        assert.gt(totalShardPeak, 0);
        assert.eq(mongosTotalPeak, totalShardPeak + mongosLocalPeak);
    });
});

describe("peakTrackedMemBytes in queryStats - mongod as router", function () {
    before(function () {
        const queryStatsParams = {
            setParameter: {
                internalQueryStatsRateLimit: -1,
                internalQueryMaxWriteToCurOpMemoryUsageBytes: 0,
            },
        };
        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {nodes: 1},
            mongosOptions: queryStatsParams,
            rsOptions: queryStatsParams,
        });
        this.mongos = this.st.s;
        this.mongosDB = this.mongos.getDB("test");
        this.shard0 = this.st.shard0;
        this.shard1 = this.st.shard1;

        assert.commandWorked(
            this.mongosDB.adminCommand({enableSharding: "test", primaryShard: this.st.shard0.shardName}),
        );

        // "local_coll" is unsharded and lives entirely on shard0.
        this.localColl = this.mongosDB["local_coll"];
        this.localColl.drop();
        const localBulk = this.localColl.initializeUnorderedBulkOp();
        for (let i = 0; i < 50; i++) {
            localBulk.insert({a: i, b: "local_" + "x".repeat(100)});
        }
        assert.commandWorked(localBulk.execute());

        // "foreign_coll" lives entirely on shard1. When shard0 processes a $lookup against
        // this collection, it must act as a router and fetch data from shard1.
        this.foreignColl = this.mongosDB["foreign_coll"];
        this.foreignColl.drop();
        assert.commandWorked(
            this.mongosDB.adminCommand({shardCollection: this.foreignColl.getFullName(), key: {a: 1}}),
        );
        assert.commandWorked(
            this.mongosDB.adminCommand({
                moveChunk: this.foreignColl.getFullName(),
                find: {a: 0},
                to: this.st.shard1.shardName,
            }),
        );
        const foreignBulk = this.foreignColl.initializeUnorderedBulkOp();
        // TODO SERVER-122701 Change foreign collection to have > 101 documents to have a getMore on $lookup and $unionWith and verify metrics still make sense.
        for (let i = 0; i < 50; i++) {
            foreignBulk.insert({a: i, b: "foreign_" + "x".repeat(100)});
        }
        assert.commandWorked(foreignBulk.execute());
    });

    after(function () {
        this.st.stop();
    });

    beforeEach(function () {
        // Clear cache before each test.
        for (const node of [this.shard0, this.shard1, this.mongos]) {
            assert.commandWorked(node.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "0MB"}));
            assert.commandWorked(node.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "1MB"}));
        }
    });

    it("$lookup against sharded foreign collection makes mongod act as router", function () {
        this.localColl
            .aggregate([
                {
                    $lookup: {
                        from: "foreign_coll",
                        let: {localA: "$a"},
                        pipeline: [{$match: {$expr: {$eq: ["$a", "$$localA"]}}}, {$sort: {b: 1}}],
                        as: "matched",
                    },
                },
                {$project: {a: 1, matchCount: {$size: "$matched"}}},
                {$sort: {a: -1}},
            ])
            .toArray();

        const shard1Stats = getQueryStatsAggCmd(this.shard1.getDB("test"), {collName: "foreign_coll"});
        assert.eq(shard1Stats.length, 1);
        const shard1Exec = getQueryExecMetrics(shard1Stats[0].metrics);
        const shard1Peak = shard1Exec.peakTrackedMemBytes.sum;
        const shard1TotalPeak = shard1Exec.clusterPeakTrackedMemBytes.sum;

        assert(shard1Peak > 0);
        assert.eq(shard1TotalPeak, shard1Peak);

        // Shard0 is acting as a router for $lookup.
        const shard0Stats = getQueryStatsAggCmd(this.shard0.getDB("test"), {collName: "local_coll"});
        assert.eq(shard0Stats.length, 1);
        const shard0Exec = getQueryExecMetrics(shard0Stats[0].metrics);
        const shard0Peak = shard0Exec.peakTrackedMemBytes.sum;
        const shard0TotalPeak = shard0Exec.clusterPeakTrackedMemBytes.sum;

        assert.gte(shard0TotalPeak, shard0Peak);
        assert.eq(shard0TotalPeak, shard1TotalPeak + shard0Peak);

        const mongosStats = getQueryStatsAggCmd(this.mongosDB, {collName: "local_coll"});
        assert.eq(mongosStats.length, 1);
        const mongosExec = getQueryExecMetrics(mongosStats[0].metrics);
        const mongosTotalPeak = mongosExec.clusterPeakTrackedMemBytes.sum;

        // Mongos metrics should reflect the metrics from shard0.
        assert.gte(mongosTotalPeak, shard0TotalPeak);
    });

    it("$unionWith against sharded collection makes mongod act as router", function () {
        this.localColl
            .aggregate([{$unionWith: {coll: "foreign_coll", pipeline: [{$sort: {b: 1}}]}}, {$sort: {a: -1}}])
            .toArray();

        const shard1Stats = getQueryStatsAggCmd(this.shard1.getDB("test"), {collName: "foreign_coll"});
        assert.eq(shard1Stats.length, 1);
        const shard1Exec = getQueryExecMetrics(shard1Stats[0].metrics);
        const shard1Peak = shard1Exec.peakTrackedMemBytes.sum;
        const shard1TotalPeak = shard1Exec.clusterPeakTrackedMemBytes.sum;

        assert(shard1Peak > 0);
        assert.eq(shard1TotalPeak, shard1Peak);

        const shard0Stats = getQueryStatsAggCmd(this.shard0.getDB("test"), {collName: "local_coll"});
        assert.eq(shard0Stats.length, 1);
        const shard0Exec = getQueryExecMetrics(shard0Stats[0].metrics);
        const shard0Peak = shard0Exec.peakTrackedMemBytes.sum;
        const shard0TotalPeak = shard0Exec.clusterPeakTrackedMemBytes.sum;

        // Shard0 is acting as a router for $lookup so it should include shard1 metrics.
        assert(shard0TotalPeak > shard0Peak);
        assert.eq(shard0TotalPeak, shard1TotalPeak + shard0Peak);

        const mongosStats = getQueryStatsAggCmd(this.mongosDB, {collName: "local_coll"});
        assert.eq(mongosStats.length, 1);
        const mongosExec = getQueryExecMetrics(mongosStats[0].metrics);
        const mongosTotalPeak = mongosExec.clusterPeakTrackedMemBytes.sum;

        // Mongos metrics should reflect the metrics from shard0.
        assert.gte(mongosTotalPeak, shard0TotalPeak);
    });
});
