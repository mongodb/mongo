/**
 * Tests creating and dropping timeseries collections along with FCV upgrade/downgrade.
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

export const $config = (function () {
    const states = {
        init: function (db, collName) {
            this.counter = 0;
        },
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
        createAndDrop: function (db, collName) {
            collName = `${jsTestName()}_tid${this.tid}_c${this.counter++}`;
            assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "t"}}));
            assert(db[collName].drop());
        },
    };

    const teardown = function (db, collName) {
        // Verify that all collections have been fully dropped.
        const remaining = db.getCollectionInfos({name: {$regex: new RegExp(jsTestName())}});
        assert.eq(remaining.length, 0, tojson(remaining));

        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    };

    return {
        threadCount: 4,
        iterations: 100,
        startState: "init",
        states,
        transitions: uniformDistTransitions(states),
        teardown,
    };
})();
