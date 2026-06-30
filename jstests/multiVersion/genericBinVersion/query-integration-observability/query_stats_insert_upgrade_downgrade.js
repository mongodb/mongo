/**
 * Verifies that query stats for inserts behaves correctly in FCV upgrade/downgrade scenarios.
 * Query stats for inserts is FCV-gated, so we expect insert stats to be present only when
 * fully upgraded.
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";
import {
    assertCommandNotRecorded,
    assertCommandRecorded,
    assertCommandRecordedOnShardsExceptRouter,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getQueryStatsInsertCmd,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();

function setupCollection(primaryConnection, shardingTest = null) {
    const db = primaryConnection.getDB(jsTestName());
    const coll = assertDropAndRecreateCollection(db, collName);

    // No documents are being inserted initially. The collection is sharded across 2 shards,
    // allowing inserts in upcoming tests to be distributed deterministically:
    // shard0: [MinKey, 0) -> _id < 0
    // shard1: [0, MaxKey] -> _id >= 0
    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1} /*key*/, {_id: 0} /*split*/, {_id: 0} /*move*/);
    }
}

function assertInsertQueryStatsMetrics(entry) {
    assert.eq(entry.key.queryShape.command, "insert");
    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 0,
        docsExamined: 0,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 0,
            nInserted: 1,
            nUpdateOps: 0,
            nDeleteOps: 0,
            keysInserted: 1,
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
 * Asserts that insert commands are not recorded in query stats.
 */
function assertInsertCommandNotRecorded(primaryConnection) {
    assertCommandNotRecorded(
        // Intentionally left unscoped (no collName filter): this negative assertion is most useful
        // when it is broad, so it also catches any unexpected shell-issued insert that leaked into
        // the store (e.g. an internal write recorded while the feature flag was enabled).
        primaryConnection,
        (db) => db.getCollection(collName).insertOne({a: 1}),
        (db) => getQueryStatsInsertCmd(db),
    );
}

/**
 * Asserts that insert commands are recorded in query stats.
 */
function assertInsertCommandRecorded(primaryConnection) {
    // We are interested only in the query stats for the test collection, so we restrict by
    // collection name explicitly. Here that restriction is functionally redundant, since we
    // just reset the query stats store; we keep it to express the intent clearly and to stay
    // correct even if other shell-issued inserts reach this node.
    assertCommandRecorded(
        primaryConnection,
        (db) => db.getCollection(collName).insertOne({a: 1}),
        (db) => getQueryStatsInsertCmd(db, {collName}),
        assertInsertQueryStatsMetrics,
    );
}

/**
 * Asserts that insert commands are recorded in query stats on all shard query stores except the router.
 */
function assertInsertCommandRecordedOnShardsExceptRouter(primaryConnection) {
    // Scope to the test collection, to filter out operations on the FCV document.
    // This is to ensure that the additional inserts performed during the upgrade/downgrade
    // process are not included in the query stat results.
    let count = 0;
    assertCommandRecordedOnShardsExceptRouter(
        primaryConnection,
        [
            (db) => db.getCollection(collName).insertOne({_id: -++count, a: 1}),
            (db) => db.getCollection(collName).insertOne({_id: ++count, a: 1}),
        ],
        (db) => getQueryStatsInsertCmd(db, {collName}),
        assertInsertQueryStatsMetrics,
    );
}

describe("Query Stats metrics should only be recorded when FCV is bumped", function () {
    it("Should only appear in a fully upgraded replica set", function () {
        testPerformUpgradeReplSet({
            upgradeNodeOptions: {
                setParameter: {
                    internalQueryStatsSampleRate: 1,
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
            setupFn: setupCollection,
            whenFullyDowngraded: assertInsertCommandNotRecorded,
            whenSecondariesAreLatestBinary: assertInsertCommandNotRecorded,
            whenBinariesAreLatestAndFCVIsLastLTS: assertInsertCommandNotRecorded,
            whenFullyUpgraded: assertInsertCommandRecorded,
        });
    });

    /**
     * Query stats for inserts is FCV-gated, but mongos pins itself to the latest FCV
     * meaning that when mongos is on the latest binary and has been restarted, it will record query
     * stats even if the cluster's FCV is last-LTS. This means that a full FCV downgrade to
     * last-LTS and mongos restart will result in query stats being recorded, contrary to expectation.
     */
    it("Should appear in a fully upgraded cluster, or when binaries are latest", function () {
        testPerformUpgradeSharded({
            upgradeNodeOptions: {
                setParameter: {
                    internalQueryStatsSampleRate: 1,
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
            setupFn: setupCollection,
            whenFullyDowngraded: assertInsertCommandNotRecorded,
            whenOnlyConfigIsLatestBinary: assertInsertCommandNotRecorded,
            whenSecondariesAndConfigAreLatestBinary: assertInsertCommandNotRecorded,
            whenMongosBinaryIsLastLTS: assertInsertCommandNotRecorded,
            whenBinariesAreLatestAndFCVIsLastLTS: assertInsertCommandRecorded,
            whenFullyUpgraded: assertInsertCommandRecordedOnShardsExceptRouter,
        });
    });
});
