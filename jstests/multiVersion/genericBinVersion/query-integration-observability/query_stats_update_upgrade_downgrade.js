/**
 * Verifies that the query stats for updates behave correctly in FCV upgrade/downgrade scenarios.
 *
 * @tags: [requires_fcv_83]
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";
import {assertAggregatedMetricsSingleExec, assertExpectedResults} from "jstests/libs/query/query_stats_utils.js";
import {getQueryStatsUpdateCmd, resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const collName = jsTestName();
const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());
const docs = [
    {_id: 0, a: 100},
    {_id: 1, a: 101},
    {_id: 2, a: 102},
    {_id: 3, a: 103},
    {_id: 4, a: 104},
    {_id: 5, a: 105},
    {_id: 6, a: 106},
    {_id: 7, a: 107},
    {_id: 8, a: 108},
    {_id: 9, a: 109},
];

function setupCollection(primaryConnection, shardingTest = null) {
    const db = getDB(primaryConnection);
    const coll = assertDropAndRecreateCollection(db, collName);

    assert.commandWorked(coll.insertMany(docs));

    // Shard the collection to get even distribution across 2 shards.
    // shard 0: {_id: 0, ..., 4}
    // shard 1: {_id: 5, ..., 9}
    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1} /*key*/, {_id: docs.length / 2} /*split*/);

        const shardedDataDistribution = shardingTest.s
            .getDB("admin")
            .aggregate([{$shardedDataDistribution: {}}, {$match: {ns: coll.getFullName()}}])
            .toArray();
        assert.eq(shardedDataDistribution.length, 1);
        assert.eq(
            shardedDataDistribution[0].shards.length,
            2,
            "Expected 2 shards in data distribution" + tojson(shardedDataDistribution),
        );
        assert.eq(
            shardedDataDistribution[0].shards[0].numOwnedDocuments,
            docs.length / 2,
            `Expected ${docs.length / 2} docs on shard 0 in data distribution` + tojson(shardedDataDistribution),
        );
        assert.eq(
            shardedDataDistribution[0].shards[1].numOwnedDocuments,
            docs.length / 2,
            `Expected ${docs.length / 2} docs on shard 1 in data distribution` + tojson(shardedDataDistribution),
        );
    }
}

/**
 * Asserts that update commands are not recorded in query stats.
 *
 * Note that if FCV is downgrade without restarting the server binaries, the previously recorded query stats will
 * still persist in the query stores. Therefore, we reset the query stats store at the beginning of this function
 * to ensure a clean state.
 */
function assertUpdateCommandsNotRecorded(primaryConn) {
    const db = getDB(primaryConn);
    resetQueryStatsStore(db, "1MB");
    assert.commandWorked(db.getCollection(collName).update({_id: 1}, {$inc: {a: 1}}));
    const queryStats = getQueryStatsUpdateCmd(db);
    assert.eq(queryStats, [], "Expected no new query stats entries, but found some");
}

function assertUpdateQueryStatsMetrics(entry) {
    assert.eq(entry.key.queryShape.command, "update");
    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 1,
        docsExamined: 1,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {nMatched: 1, nUpserted: 0, nModified: 1, nDeleted: 0, nInserted: 0, nUpdateOps: 1},
    });
    assertExpectedResults({
        results: entry,
        expectedQueryStatsKey: entry.key,
        expectedExecCount: 1,
        expectedDocsReturnedSum: 0,
        expectedDocsReturnedMax: 0,
        expectedDocsReturnedMin: 0,
        expectedDocsReturnedSumOfSq: 0,
    });
}

/**
 * Asserts that update commands are recorded in query stats.
 */
function assertUpdateCommandRecorded(primaryConn) {
    const db = getDB(primaryConn);
    resetQueryStatsStore(db, "1MB");
    assert.commandWorked(db.getCollection(collName).update({_id: 1}, {$inc: {a: 1}}));
    const queryStats = getQueryStatsUpdateCmd(db);
    assert.neq(queryStats, [], "Expected query stats entry for update command, but found none");
    assert.eq(queryStats.length, 1, "Expected exactly one query stats entry for update command");
    const entry = queryStats[0];
    assertUpdateQueryStatsMetrics(entry);
}

/**
 * Given an initial connection to the cluster, applies the given function on all shard servers.
 * @param {*} primaryConn
 * @param {*} perShardFn Function to apply on each shard server connection.
 */
function applyOnShardServers(primaryConn, perShardFn) {
    const topology = DiscoverTopology.findConnectedNodes(primaryConn);
    assert.eq(topology.type, Topology.kShardedCluster);

    // Sharded cluster - run on all shard nodes as well.
    for (let shardName of Object.keys(topology.shards)) {
        const shard = topology.shards[shardName];
        if (shard.type === Topology.kReplicaSet) {
            // Await replication to ensure all of the shards are queryable.
            const rst = new ReplSetTest(shard.primary);
            rst.awaitReplication();
            perShardFn(newMongoWithRetry(shard.primary));
        } else if (shard.type === Topology.kStandalone) {
            perShardFn(newMongoWithRetry(shard.mongod));
        }
    }
}

/**
 * Asserts that update commands are recorded in query stats on all shard servers except the router.
 */
function assertUpdateCommandRecordedOnShardsExceptRouter(primaryConn) {
    const db = getDB(primaryConn);
    resetQueryStatsStore(db, "1MB");
    // Apply updates on one document for each shard
    assert.commandWorked(db.getCollection(collName).update({_id: 0}, {$inc: {a: 1}}));
    assert.commandWorked(db.getCollection(collName).update({_id: 6}, {$inc: {a: 1}}));

    const assertRecordedOnShardServer = (conn) => {
        const db = getDB(conn);
        const queryStats = getQueryStatsUpdateCmd(db);
        assert.neq(queryStats, [], "Expected query stats entry for update command, but found none");
        assert.eq(queryStats.length, 1, "Expected exactly one query stats entry for update command");
        const entry = queryStats[0];
        assertUpdateQueryStatsMetrics(entry);
    };

    applyOnShardServers(primaryConn, assertRecordedOnShardServer);
}

/**
 * featureFlagQueryStatsUpdateCommand is fcv-gated. We expect that update commands are only recorded when FCV is bumped.
 * TODO: We will revisit this part when 8.3 becomes the last LTS.
 */
testPerformUpgradeReplSet({
    upgradeNodeOptions: {setParameter: {featureFlagQueryStatsUpdateCommand: true, internalQueryStatsSampleRate: 1}},
    setupFn: setupCollection,
    whenFullyDowngraded: assertUpdateCommandsNotRecorded,
    whenSecondariesAreLatestBinary: assertUpdateCommandsNotRecorded,
    whenBinariesAreLatestAndFCVIsLastLTS: assertUpdateCommandsNotRecorded,
    whenFullyUpgraded: assertUpdateCommandRecorded,
});

/**
 * featureFlagQueryStatsUpdateCommand only enables the query stats for updates on shard servers rather than routers.
 * So we expect that there is no update commands being recorded on mongos. Having said that, this test is still useful
 * to check that there is no regression when shard servers send cursor metrics using the latest UpdateCommandReply.
 */
testPerformUpgradeSharded({
    upgradeNodeOptions: {setParameter: {featureFlagQueryStatsUpdateCommand: true, internalQueryStatsSampleRate: 1}},
    setupFn: setupCollection,
    whenFullyDowngraded: assertUpdateCommandsNotRecorded,
    whenOnlyConfigIsLatestBinary: assertUpdateCommandsNotRecorded,
    whenSecondariesAndConfigAreLatestBinary: assertUpdateCommandsNotRecorded,
    whenMongosBinaryIsLastLTS: assertUpdateCommandsNotRecorded,
    whenBinariesAreLatestAndFCVIsLastLTS: assertUpdateCommandsNotRecorded,
    whenFullyUpgraded: assertUpdateCommandRecordedOnShardsExceptRouter,
});
