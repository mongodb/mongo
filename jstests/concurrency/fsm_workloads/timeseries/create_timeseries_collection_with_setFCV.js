/**
 * Stress test idempotency of creates time-series collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # Runs setFCV, which can interfere with other tests.
 *   incompatible_with_concurrency_simultaneous,
 *   runs_set_fcv,
 *   # TODO (SERVER-104171) Remove the 'assumes_balancer_off' tag
 *   assumes_balancer_off,
 *   # The fuzzer tries to enable server parameters not available on lastLTS FCV
 *   does_not_support_config_fuzzer,
 * ]
 */

import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
const timeFieldName = "t_field";
const metaFieldName = "m_field";
const maxNumCollections = 1;

export const $config = (function () {
    let getRandomColl = function () {
        return `coll_${Random.randInt(maxNumCollections)}`;
    };

    let states = {
        setFCV: (db, collName) => {
            const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
            const targetFCV = fcvValues[Random.randInt(3)];
            jsTest.log.info("Executing FCV state, setting to:" + targetFCV);
            try {
                assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
            } catch (e) {
                if (handleRandomSetFCVErrors(e, targetFCV)) return;
                throw e;
            }
            jsTest.log.info("setFCV state finished");
        },
        create: (db, collName) => {
            const coll = db[getRandomColl()];
            assert.commandWorkedOrFailedWithCode(
                db.createCollection(coll.getName(), {
                    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
                }),
                [
                    // In suites that implicitly shard or track the timeseries collection,
                    // the router could exhaust the retry attempts on StaleConfig because we repeatedly
                    // drop and re-create the collection, causing its shard version to change continuously.
                    ErrorCodes.StaleConfig,
                ],
            );
        },
        drop: (db, collName) => {
            const coll = db[getRandomColl()];
            assert.commandWorked(db.runCommand({drop: coll.getName()}));
        },
    };

    let teardown = function (db, collName) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    };

    return {
        threadCount: 5,
        startState: "create",
        iterations: 300,
        states: states,
        teardown: teardown,
        transitions: uniformDistTransitions(states),
    };
})();
