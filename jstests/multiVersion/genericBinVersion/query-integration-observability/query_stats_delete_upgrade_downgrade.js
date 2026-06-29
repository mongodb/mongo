/**
 * Verifies that the query stats for deletes behave correctly in FCV upgrade/downgrade scenarios.
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {testPerformUpgradeReplSet} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {testPerformUpgradeSharded} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getQueryStatsDeleteCmd,
} from "jstests/libs/query/query_stats_utils.js";
import {
    assertCommandNotRecorded,
    assertCommandRecorded,
    assertCommandRecordedOnShardsExceptRouter,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";

const collName = jsTestName();
const docs = Array.from({length: 100}, (_, i) => ({_id: i, a: 100 + i}));

function setupCollection(primaryConnection, shardingTest = null) {
    const db = primaryConnection.getDB(jsTestName());
    const coll = assertDropAndRecreateCollection(db, collName);

    assert.commandWorked(coll.insertMany(docs));

    // Shard the collection to get even distribution across 2 shards.
    // shard 0: {_id: 0, ..., 49}
    // shard 1: {_id: 50, ..., 99}
    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1} /*key*/, {_id: docs.length / 2} /*split*/);

        const shardedDataDistribution = shardingTest.s
            .getDB("admin")
            .aggregate([{$shardedDataDistribution: {}}, {$match: {ns: db[collName].getFullName()}}])
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
 * Asserts that delete commands are not recorded in query stats.
 */
function assertDeleteCommandsNotRecorded(primaryConn) {
    assertCommandNotRecorded(
        primaryConn,
        (db) => db.getCollection(collName).deleteOne({_id: -1}),
        // Intentionally left unscoped (no collName filter): this negative assertion is most useful
        // when it is broad, so it also catches any unexpected shell-issued delete that leaked into
        // the store (e.g. an internal write recorded while the feature flag was enabled).
        (db) => getQueryStatsDeleteCmd(db),
    );
}

function assertDeleteQueryStatsMetrics(entry) {
    assert.eq(entry.key.queryShape.command, "delete");
    assertAggregatedMetricsSingleExec(entry, {
        keysExamined: 1,
        docsExamined: 1,
        hasSortStage: false,
        usedDisk: false,
        fromMultiPlanner: false,
        fromPlanCache: false,
        writes: {
            nMatched: 0,
            nUpserted: 0,
            nModified: 0,
            nDeleted: 1,
            nInserted: 0,
            nUpdateOps: 0,
            nDeleteOps: 1,
            keysInserted: 0,
            keysDeleted: 1,
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
 * Asserts that delete commands are recorded in query stats.
 */
function assertDeleteCommandRecorded(primaryConn) {
    // We are interested only in the query stats for the test collection, so we restrict by
    // collection name explicitly. Here that restriction is functionally redundant, since we
    // just reset the query stats store; we keep it to express the intent clearly and to stay
    // correct even if other shell-issued deletes reach this node.
    assertCommandRecorded(
        primaryConn,
        (db) => {
            // This callback may be invoked multiple times by the fixture, so we fetch a document on
            // each invocation to ensure the delete command removes exactly one document and the
            // query stats metrics are correct.
            const doc = db.getCollection(collName).findOne({});
            assert.neq(doc, null, "No document available to delete", {collName});
            return db.getCollection(collName).deleteOne({_id: doc._id});
        },
        (db) => getQueryStatsDeleteCmd(db, {collName}),
        assertDeleteQueryStatsMetrics,
    );
}

/**
 * Asserts that delete commands are recorded in query stats on all shard servers except the router.
 */
function assertDeleteCommandRecordedOnShardsExceptRouter(primaryConn) {
    // Scope to the test collection, to filter out operations on the FCV document.
    // This is to ensure that the additional deletes performed on the FCV document during
    // the upgrade/downgrade process are not included in the query stat results.
    const splitPoint = docs.length / 2;
    assertCommandRecordedOnShardsExceptRouter(
        primaryConn,
        [
            (db) => {
                const doc = db.getCollection(collName).findOne({_id: {$lt: splitPoint}});
                assert.neq(doc, null, "No document available to delete in lower chunk", {collName});
                return db.getCollection(collName).deleteOne({_id: doc._id});
            },
            (db) => {
                const doc = db.getCollection(collName).findOne({_id: {$gte: splitPoint}});
                assert.neq(doc, null, "No document available to delete in upper chunk", {collName});
                return db.getCollection(collName).deleteOne({_id: doc._id});
            },
        ],
        (db) => getQueryStatsDeleteCmd(db, {collName}),
        assertDeleteQueryStatsMetrics,
    );
}

/**
 * featureFlagQueryStatsDelete is fcv-gated. We expect that delete commands are only recorded when
 * FCV is bumped.
 */
testPerformUpgradeReplSet({
    upgradeNodeOptions: {
        setParameter: {
            // TODO SERVER-123427 remove enabling featureFlagQueryStatsDelete.
            featureFlagQueryStatsDelete: true,
            internalQueryStatsSampleRate: 1,
            internalQueryStatsWriteCmdSampleRate: 1,
        },
    },
    setupFn: setupCollection,
    whenFullyDowngraded: assertDeleteCommandsNotRecorded,
    whenSecondariesAreLatestBinary: assertDeleteCommandsNotRecorded,
    whenBinariesAreLatestAndFCVIsLastLTS: assertDeleteCommandsNotRecorded,
    whenFullyUpgraded: assertDeleteCommandRecorded,
});

/**
 * featureFlagQueryStatsDelete is FCV-gated, but mongos pins itself to the latest FCV, so once
 * mongos is on the latest binary a delete routed through it is recorded in the router's query stats
 * even while the cluster FCV is still last-LTS. After the FCV is bumped, the shards take over
 * recording instead of the router. This also checks that there is no regression when shard servers
 * send cursor metrics using the latest DeleteCommandReply.
 */
testPerformUpgradeSharded({
    upgradeNodeOptions: {
        setParameter: {
            // TODO SERVER-123427 remove enabling featureFlagQueryStatsDelete.
            featureFlagQueryStatsDelete: true,
            internalQueryStatsSampleRate: 1,
            internalQueryStatsWriteCmdSampleRate: 1,
        },
    },
    setupFn: setupCollection,
    whenFullyDowngraded: assertDeleteCommandsNotRecorded,
    whenOnlyConfigIsLatestBinary: assertDeleteCommandsNotRecorded,
    whenSecondariesAndConfigAreLatestBinary: assertDeleteCommandsNotRecorded,
    whenMongosBinaryIsLastLTS: assertDeleteCommandsNotRecorded,
    whenBinariesAreLatestAndFCVIsLastLTS: assertDeleteCommandRecorded,
    whenFullyUpgraded: assertDeleteCommandRecordedOnShardsExceptRouter,
});
