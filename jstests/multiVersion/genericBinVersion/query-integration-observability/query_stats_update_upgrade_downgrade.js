/**
 * Verifies that the query stats for updates behave correctly in FCV upgrade/downgrade scenarios.
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getQueryStatsUpdateCmd,
} from "jstests/libs/query/query_stats_utils.js";
import {
    assertCommandNotRecorded,
    assertCommandRecorded,
    assertCommandRecordedOnShardsExceptRouter,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";

const collName = jsTestName();
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
    const db = primaryConnection.getDB(jsTestName());
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
            `Expected ${docs.length / 2} docs on shard 0 in data distribution` +
                tojson(shardedDataDistribution),
        );
        assert.eq(
            shardedDataDistribution[0].shards[1].numOwnedDocuments,
            docs.length / 2,
            `Expected ${docs.length / 2} docs on shard 1 in data distribution` +
                tojson(shardedDataDistribution),
        );
    }
}

/**
 * Asserts that update commands are not recorded in query stats.
 */
function assertUpdateCommandsNotRecorded(primaryConn) {
    assertCommandNotRecorded(
        // Intentionally left unscoped (no collName filter): this negative assertion is most useful
        // when it is broad, so it also catches any unexpected shell-issued update that leaked into
        // the store (e.g. an internal write recorded while the feature flag was enabled).
        primaryConn,
        (db) => db.getCollection(collName).update({_id: 1}, {$inc: {a: 1}}),
        (db) => getQueryStatsUpdateCmd(db),
    );
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
        writes: {
            nMatched: 1,
            nUpserted: 0,
            nModified: 1,
            nDeleted: 0,
            nInserted: 0,
            nUpdateOps: 1,
            nDeleteOps: 0,
            // The update touches the non-indexed field 'a', so no index keys are maintained.
            keysInserted: 0,
            keysDeleted: 0,
        },
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
    // We are interested only in the query stats for the test collection, so we restrict by
    // collection name explicitly. Here that restriction is functionally redundant, since we
    // just reset the query stats store; we keep it to express the intent clearly and to stay
    // correct even if other shell-issued updates reach this node.
    assertCommandRecorded(
        primaryConn,
        (db) => db.getCollection(collName).update({_id: 1}, {$inc: {a: 1}}),
        (db) => getQueryStatsUpdateCmd(db, {collName}),
        assertUpdateQueryStatsMetrics,
    );
}

/**
 * Asserts that update commands are recorded in query stats on all shard servers except the router.
 */
function assertUpdateCommandRecordedOnShardsExceptRouter(primaryConn) {
    // Scope to the test collection, to filter out operations on the FCV document.
    // This is to ensure that the additional updates performed on the FCV document during
    // the upgrade/downgrade process are not included in the query stat results.
    assertCommandRecordedOnShardsExceptRouter(
        primaryConn,
        [
            (db) => db.getCollection(collName).update({_id: 0}, {$inc: {a: 1}}),
            (db) => db.getCollection(collName).update({_id: 6}, {$inc: {a: 1}}),
        ],
        (db) => getQueryStatsUpdateCmd(db, {collName}),
        assertUpdateQueryStatsMetrics,
    );
}

/**
 * featureFlagQueryStatsUpdateCommand is fcv-gated. We expect that update commands are only recorded when FCV is bumped.
 * TODO: We will revisit this part when 8.3 becomes the last LTS.
 */
testPerformUpgradeReplSet({
    upgradeNodeOptions: {
        setParameter: {
            featureFlagQueryStatsUpdateCommand: true,
            internalQueryStatsSampleRate: 1,
            internalQueryStatsWriteCmdSampleRate: 1,
        },
    },
    setupFn: setupCollection,
    whenFullyDowngraded: assertUpdateCommandsNotRecorded,
    whenSecondariesAreLatestBinary: assertUpdateCommandsNotRecorded,
    whenBinariesAreLatestAndFCVIsLastLTS: assertUpdateCommandsNotRecorded,
    whenFullyUpgraded: assertUpdateCommandRecorded,
});

/**
 * featureFlagQueryStatsUpdateCommand is FCV-gated, but mongos pins itself to the latest FCV, so once
 * mongos is on the latest binary an update routed through it is recorded in the router's query stats
 * even while the cluster FCV is still last-LTS. After the FCV is bumped, the shards take over
 * recording instead of the router. This also checks that there is no regression when shard servers
 * send cursor metrics using the latest UpdateCommandReply.
 */
testPerformUpgradeSharded({
    upgradeNodeOptions: {
        setParameter: {
            featureFlagQueryStatsUpdateCommand: true,
            internalQueryStatsSampleRate: 1,
            internalQueryStatsWriteCmdSampleRate: 1,
        },
    },
    setupFn: setupCollection,
    whenFullyDowngraded: assertUpdateCommandsNotRecorded,
    whenOnlyConfigIsLatestBinary: assertUpdateCommandsNotRecorded,
    whenSecondariesAndConfigAreLatestBinary: assertUpdateCommandsNotRecorded,
    whenMongosBinaryIsLastLTS: assertUpdateCommandsNotRecorded,
    whenBinariesAreLatestAndFCVIsLastLTS: assertUpdateCommandRecorded,
    whenFullyUpgraded: assertUpdateCommandRecordedOnShardsExceptRouter,
});
