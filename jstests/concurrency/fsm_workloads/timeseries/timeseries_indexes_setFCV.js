/**
 * Tests createIndex, listIndexes, and dropIndexes return consistent results for timeseries
 * collections while upgrading/downgrading the FCV in the background.
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
 *   runs_set_fcv,
 *   creates_background_indexes,
 * ]
 */
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {isShardedTimeseries} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

export const $config = (function () {
    const indexSpec = {temp: 1};

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

        createIndex: function (db, collName) {
            const coll = db[collName];
            assert.commandWorkedOrFailedWithCode(coll.createIndex(indexSpec), [
                // Dropped while creating.
                ErrorCodes.IndexBuildAborted,
                // Collection got upgraded/downgraded while yielded during the index build.
                ErrorCodes.InterruptedDueToTimeseriesUpgradeDowngrade,
            ]);
        },

        listIndexes: function (db, collName) {
            const coll = db[collName];
            const indexes = coll.getIndexes();

            // We always find the default {m: 1, t: 1} index.
            assert(indexes.map((i) => i.name).includes("m_1_t_1"), tojson(indexes));

            // We may also find:
            // - The shard key index.
            // - The index we create & drop.
            const maxIndexes = 2 + (isShardedTimeseries(db[collName]) ? 1 : 0);
            assert.lte(indexes.length, maxIndexes, tojson(indexes));
        },

        dropIndexes: function (db, collName) {
            const coll = db[collName];
            assert.commandWorked(coll.dropIndex(indexSpec));
        },
    };

    const setup = function (db, collName, cluster) {
        db[collName].drop();
        assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
        assert.commandWorked(db[collName].insert({t: new Date(), temp: 42}));
    };
    const teardown = function (db, collName, cluster) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    };

    return {
        threadCount: 4,
        iterations: 40,
        states,
        transitions: uniformDistTransitions(states),
        setup,
        teardown,
    };
})();
