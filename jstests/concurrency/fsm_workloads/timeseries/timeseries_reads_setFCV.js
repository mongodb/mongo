/**
 * Tests listCollections and reads (find/aggregate/...) return consistent results for timeseries
 * collections while upgrading/downgrading the FCV in the background.
 * This is designed to exercise viewless timeseries upgrade/downgrade.
 * TODO(SERVER-114573): Consider removing this test once 9.0 becomes lastLTS.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_getmore,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # TODO (SERVER-104171) Remove the 'assumes_balancer_off' tag
 *   assumes_balancer_off,
 *   # Runs setFCV, which can interfere with other tests.
 *   incompatible_with_concurrency_simultaneous,
 *   runs_set_fcv,
 *   # This test relies on certain values of 'hangBeforePublishingCatalogUpdates', which the config
 *   # fuzzer can change and cause the test to hang.
 *   does_not_support_config_fuzzer,
 * ]
 */
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {setFCVWithRetryOnBackgroundOpInProgress} from "jstests/libs/set_fcv_helpers.js";

// Errors that are expected transiently while reads run concurrently with FCV transitions:
//  - InterruptedDueToTimeseriesUpgradeDowngrade: a concurrent FCV transition interrupted the read.
//  - IngressRequestRateLimitExceeded: the rate-limited suite injects the
//    failIngressRequestRateLimiting failpoint on the shards. A read shed on a shard is rejected
//    before the command runs, and that rejection does carry the RetryableError label. mongos does
//    not forward the shard's labels though: it rebuilds them for the client, and no longer treats
//    this error as idempotent, so the label is dropped and mongos does not retry -- the error
//    reaches the shell. SERVER-128710 removed that special case deliberately (mongos may have
//    already retried, and the original request may not be idempotent); A concurrent FCV transition
//    makes this more likely, since each StaleConfig-driven routing retry is another chance to be
//    shed.
const acceptedErrors = [
    ErrorCodes.InterruptedDueToTimeseriesUpgradeDowngrade,
    ErrorCodes.IngressRequestRateLimitExceeded,
];

// Runs `func` and retries if it fails with one of the accepted transient errors.
function withRetryOnAcceptedErrors(func) {
    let result;
    assert.soonRetryOnAcceptableErrors(
        () => {
            result = func();
            return true;
        },
        acceptedErrors,
        "Timed out waiting for timeseries operation to succeed without a transient error",
    );
    return result;
}

export const $config = (function () {
    // Use the workload name as a prefix for the collection name,
    // since the workload name is assumed to be unique.
    const prefix = jsTestName();

    const numCollections = 10;
    function getCollection(db, num) {
        return db.getCollection(prefix + "_" + num);
    }

    // Generate test documents; 150 docs ensures find() uses multiple batches via getMore.
    const expectedDocs = Array.from({length: 150}, (_, i) => ({
        t: new Date(ISODate("2024-01-01T00:00:00.000Z").getTime() + i * 1000),
        temp: i,
    }));

    const states = {
        init: function (db, collName) {},

        setFCV: function (db, collName) {
            const fcvValues = [lastLTSFCV, latestFCV];
            const targetFCV = fcvValues[Random.randInt(2)];
            jsTestLog("Executing FCV state, setting to:" + targetFCV);
            try {
                assert.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}),
                );
            } catch (e) {
                if (handleRandomSetFCVErrors(e, targetFCV)) return;
                throw e;
            }
            jsTestLog("setFCV state finished");
        },

        listCollections: function (db, collName) {
            const listCollections = db.getCollectionInfos({
                name: {$regex: new RegExp(jsTestName())},
            });
            const collectionNames = new Set(listCollections.map((n) => n.name));

            // All timeseries collections used by the FSM should be visible.
            const mainCollections = listCollections.filter(
                (n) => !n.name.startsWith("system.buckets."),
            );
            assert.eq(mainCollections.length, numCollections, tojson(listCollections));

            // We should observe a consistent state: Each timeseries collection should have a
            // system.buckets namespace if and only if it is in viewful format.
            for (const coll of mainCollections) {
                const isViewfulTimeseries = coll.info.uuid == undefined;
                const hasBuckets = collectionNames.has("system.buckets." + coll.name);
                assert.eq(isViewfulTimeseries, hasBuckets, tojson(listCollections));
            }
        },

        find: function (db, collName) {
            const coll = getCollection(db, Random.randInt(numCollections));

            const actualDocs = withRetryOnAcceptedErrors(() => coll.find({}, {_id: 0}).toArray());
            assert.sameMembers(expectedDocs, actualDocs);
        },

        findWithMajority: function (db, collName) {
            const coll = getCollection(db, Random.randInt(numCollections));

            const actualDocs = withRetryOnAcceptedErrors(() =>
                coll.find({}, {_id: 0}).readConcern("majority").toArray(),
            );
            assert.sameMembers(expectedDocs, actualDocs);
        },

        findOne: function (db, collName) {
            const coll = getCollection(db, Random.randInt(numCollections));

            const doc = withRetryOnAcceptedErrors(() =>
                coll.findOne({t: expectedDocs[0].t}, {_id: 0}),
            );
            assert.eq(doc, expectedDocs[0]);
        },

        aggregate: function (db, collName) {
            const coll = getCollection(db, Random.randInt(numCollections));

            const result = withRetryOnAcceptedErrors(() =>
                coll.aggregate([{$group: {_id: null, minTemp: {$min: "$temp"}}}]).toArray(),
            );
            assert.eq(result[0].minTemp, expectedDocs[0].temp);
        },

        countDocuments: function (db, collName) {
            const coll = getCollection(db, Random.randInt(numCollections));

            const count = withRetryOnAcceptedErrors(() => coll.countDocuments({}));
            assert.eq(count, expectedDocs.length);
        },

        collStatsCmd: function (db, collName) {
            const coll = getCollection(db, Random.randInt(numCollections));

            const result = withRetryOnAcceptedErrors(() =>
                assert.commandWorked(db.runCommand({collStats: coll.getName()})),
            );
            assert.hasFields(result, ["timeseries"]);
        },

        collStatsAgg: function (db, collName) {
            const coll = getCollection(db, Random.randInt(numCollections));

            const result = withRetryOnAcceptedErrors(() =>
                coll.aggregate([{$collStats: {storageStats: {}}}]).toArray(),
            );
            assert.hasFields(result[0], ["storageStats"]);
            assert.hasFields(result[0].storageStats, ["timeseries"]);
        },
    };

    // The legacy shell does not attach readConcern.afterClusterTime to collStats, so it can read a
    // snapshot predating this workload's setup when causal consistency suites route it to a
    // secondary. Remove the state and its transitions in those suites, but retain collStats coverage
    // everywhere else.
    if (TestData.runningWithCausalConsistency) {
        delete states.collStatsCmd;
    }

    const setup = function (db, collName, cluster) {
        // Work with multiple collections to maximize the chance we find upgrade/downgrade issues
        // by spending a bigger fraction on time of setFCV on timeseries upgrade/downgrade.
        for (let i = 0; i < numCollections; i++) {
            const coll = getCollection(db, i);
            assert.commandWorked(
                db.createCollection(coll.getName(), {timeseries: {timeField: "t"}}),
            );
            assert.commandWorked(coll.insertMany(expectedDocs));
        }

        // Increase the pending commit time in the catalog to exercise the fix for SERVER-115811.
        cluster.executeOnMongodNodes((adminDb) => {
            configureFailPoint(
                adminDb,
                "hangBeforePublishingCatalogUpdates",
                {
                    pauseEntireCommitMillis: 25,
                },
                {activationProbability: 0.15},
            );
        });
    };
    const teardown = function (db, collName, cluster) {
        cluster.executeOnMongodNodes((adminDb) => {
            configureFailPoint(adminDb, "hangBeforePublishingCatalogUpdates", {}, "off");
        });

        setFCVWithRetryOnBackgroundOpInProgress(db, latestFCV);
    };

    return {
        threadCount: 4,
        iterations: 100,
        states,
        transitions: uniformDistTransitions(states),
        setup,
        teardown,
    };
})();
