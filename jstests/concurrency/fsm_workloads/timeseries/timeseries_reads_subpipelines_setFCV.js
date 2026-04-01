/**
 * Tests aggregations with subpipelines return consistent results for timeseries collections while
 * upgrading/downgrading the FCV in the background. This is designed to exercise viewless timeseries
 * upgrade/downgrade.
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
 * ]
 */

import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

// Runs `func` and retries if it is interrupted with a transient timeseries upgrade/downgrade error.
function withRetryOnTimeseriesUpgradeDowngradeError(func) {
    let result;
    // TODO SERVER-109819 remove 'InterruptedDueToTimeseriesUpgradeDowngrade' once 9.0 becomes last LTS.
    assert.soonRetryOnAcceptableErrors(
        () => {
            result = func();
            return true;
        },
        [ErrorCodes.InterruptedDueToTimeseriesUpgradeDowngrade, ErrorCodes.CollectionBecameView],
        "Timed out waiting for timeseries operation to succeed without upgrade/downgrade error",
    );
    return result;
}

export const $config = (function () {
    // Use the workload name as a prefix for the collection name, since the workload name is assumed to be unique.
    const prefix = jsTestName();

    const numCollections = 10;
    function getCollection(db, num) {
        return db.getCollection(prefix + "_" + num);
    }

    // Generate test documents; 200 docs ensures different pipeline uses multiple batches via getMore.
    const expectedDocs = Array.from({length: 200}, (_, i) => ({
        t: new Date(ISODate("2024-01-01T00:00:00.000Z").getTime() + i * 1000),
        temp: i,
        val: i + 1,
    }));

    const states = {
        init: function (db, collName) {},

        setFCV: function (db, collName) {
            const fcvValues = [lastLTSFCV, latestFCV];
            const targetFCV = fcvValues[Random.randInt(2)];
            jsTest.log.info("Executing FCV state, setting to:" + targetFCV);
            try {
                assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
            } catch (e) {
                if (handleRandomSetFCVErrors(e, targetFCV)) return;
                throw e;
            }
            jsTest.log.info("setFCV state finished");
        },

        lookup: function (db, collName) {
            const collIndex = Random.randInt(numCollections);
            const otherCollIndex = Random.randInt(numCollections);
            const coll = getCollection(db, collIndex);
            const otherColl = getCollection(db, otherCollIndex);
            const result = withRetryOnTimeseriesUpgradeDowngradeError(() =>
                coll
                    .aggregate([
                        {
                            $lookup: {
                                from: otherColl.getName(),
                                localField: "temp",
                                foreignField: "temp",
                                as: "joined",
                            },
                        },
                    ])
                    .toArray(),
            );
            assert.eq(result.length, 200, result);
            assert.gt(result[0].joined.length, 0, result);
        },

        graphLookup: function (db, collName) {
            const collIndex = Random.randInt(numCollections);
            const otherCollIndex = Random.randInt(numCollections);
            const coll = getCollection(db, collIndex);
            const otherColl = getCollection(db, otherCollIndex);
            const result = withRetryOnTimeseriesUpgradeDowngradeError(() =>
                coll
                    .aggregate([
                        {
                            $graphLookup: {
                                from: otherColl.getName(),
                                startWith: "$temp",
                                connectFromField: "temp",
                                connectToField: "temp",
                                as: "connected",
                                maxDepth: 2,
                            },
                        },
                    ])
                    .toArray(),
            );
            assert.eq(result.length, 200, result);
            assert.gt(result[0].connected.length, 0, result);
        },

        unionWith: function (db, collName) {
            const collIndex = Random.randInt(numCollections);
            const otherCollIndex = Random.randInt(numCollections);
            const coll = getCollection(db, collIndex);
            const otherColl = getCollection(db, otherCollIndex);
            const result = withRetryOnTimeseriesUpgradeDowngradeError(() =>
                coll.aggregate([{$unionWith: {coll: otherColl.getName()}}]).toArray(),
            );
            assert.eq(result.length, 400, result);
            assert(result[0].hasOwnProperty("temp"), result);
        },
    };

    const setup = function (db, collName, cluster) {
        // Work with multiple collections to maximize the chance we find upgrade/downgrade issues
        // by spending a bigger fraction on time of setFCV on timeseries upgrade/downgrade.
        for (let i = 0; i < numCollections; i++) {
            const coll = getCollection(db, i);
            assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "t"}}));
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

        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
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
