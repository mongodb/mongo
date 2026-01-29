/**
 * Tests listCollections returns consistent results for timeseries collections
 * while upgrading/downgrading in the background.
 * This is designed to exercise viewless timeseries upgrade/downgrade.
 * TODO(SERVER-114573): Consider removing this test once 9.0 becomes lastLTS.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # TODO (SERVER-104171) Remove the 'assumes_balancer_off' tag
 *   assumes_balancer_off,
 *   # Runs setFCV, which can interfere with other tests.
 *   incompatible_with_concurrency_simultaneous,
 * ]
 */
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

export const $config = (function () {
    // Use the workload name as a prefix for the collection name,
    // since the workload name is assumed to be unique.
    const prefix = jsTestName();

    const numCollections = 10;
    function getCollection(db, num) {
        return db.getCollection(prefix + "_" + num);
    }

    const states = {
        init: function (db, collName) {},

        setFCV: function (db, collName) {
            const fcvValues = [lastLTSFCV, latestFCV];
            const targetFCV = fcvValues[Random.randInt(2)];
            jsTestLog("Executing FCV state, setting to:" + targetFCV);
            try {
                assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
            } catch (e) {
                if (handleRandomSetFCVErrors(e, targetFCV)) return;
                throw e;
            }
            jsTestLog("setFCV state finished");
        },

        listCollections: function (db, collName) {
            const listCollections = db.getCollectionInfos({name: {$regex: new RegExp(jsTestName())}});
            const collectionNames = new Set(listCollections.map((n) => n.name));

            // All timeseries collections used by the FSM should be visible.
            const mainCollections = listCollections.filter((n) => !n.name.startsWith("system.buckets."));
            assert.eq(mainCollections.length, numCollections, tojson(listCollections));

            // We should observe a consistent state: Each timeseries collection should have a
            // system.buckets namespace if and only if it is in viewful format.
            for (const coll of mainCollections) {
                const isViewfulTimeseries = coll.info.uuid == undefined;
                const hasBuckets = collectionNames.has("system.buckets." + coll.name);
                assert.eq(isViewfulTimeseries, hasBuckets, tojson(listCollections));
            }
        },
    };

    const setup = function (db, collName, cluster) {
        // Work with multiple collections to maximize the chance we find upgrade/downgrade issues
        // by spending a bigger fraction on time of setFCV on timeseries upgrade/downgrade.
        for (let i = 0; i < numCollections; i++) {
            const coll = getCollection(db, i);
            assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "t"}}));
        }

        // Increase the pending commit time in the catalog to exercise the fix for SERVER-115811.
        cluster.executeOnMongodNodes((adminDb) => {
            configureFailPoint(adminDb, "hangBeforePublishingCatalogUpdates", {
                pauseEntireCommitMillis: 25,
            });
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
        iterations: 30,
        states,
        transitions: uniformDistTransitions(states),
        setup,
        teardown,
    };
})();
