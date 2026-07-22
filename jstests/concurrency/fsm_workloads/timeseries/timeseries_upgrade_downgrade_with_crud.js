/**
 * Test that CRUD operations on timeseries collection works correctly
 * when executed concurrently with FCV upgrade/downgrade.
 *
 * TODO SERVER-117477: remove this test once 9.0 becomes lastLTS
 * by then we will not perform any timeseries transformation on FCV upgrade/downgrade.
 *
 * @tags: [
 *  requires_timeseries,
 *  # Requires all nodes to be running the latest binary.
 *  multiversion_incompatible,
 *  # Runs setFCV, which can interfere with other tests.
 *  incompatible_with_concurrency_simultaneous,
 *  # Suites with balancer don't support retriable commands outside of non-retriable sessions (e.g. delete)
 *  assumes_balancer_off,
 *  # Suites with stepdowns don't support retriable commands outside of non-retriable writes (e.g. delete)
 *  does_not_support_stepdowns,
 *  # This test performs FCV upgrade/downgrade, and config fuzzer
 *  # may set cluster/server parameters incompatible with the current/target FCV
 *  does_not_support_config_fuzzer,
 *  runs_set_fcv,
 *  requires_getmore,
 * ]
 */

import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

const timeFieldName = "t_field";
const metaFieldName = "m_field";

export const $config = (function () {
    let getCollNames = function () {
        if (Random.randInt(2)) {
            return ["A_coll", "B_coll"];
        } else {
            return ["B_coll", "A_coll"];
        }
    };

    let rndMeta = function () {
        const meta_values = ["x", "y", "z"];
        return meta_values[Random.randInt(3)];
    };

    // Errors that are expected transiently while CRUD runs concurrently with FCV transitions in the
    // rate-limited suite, and which the workload should tolerate rather than fail on:
    //  - InterruptedDueToTimeseriesUpgradeDowngrade: a concurrent FCV transition interrupted the op.
    //  - IngressRequestRateLimitExceeded: the rate-limited suite injects the
    //    failIngressRequestRateLimiting failpoint on the shards. Reads are not labeled
    //    RetryableError (they are not marked idempotent; SERVER-108898 is not planned), so shed
    //    reads are not retried by mongos and surface directly to the shell. Writes can likewise
    //    surface this error once mongos exhausts its internal retries. This is expected behavior in
    //    the rate-limited suite, not a transient FCV artifact.
    const commonAcceptedErrors = [
        ErrorCodes.InterruptedDueToTimeseriesUpgradeDowngrade,
        ErrorCodes.IngressRequestRateLimitExceeded,
    ];

    // Runs 'fn', swallowing the commonly-accepted transient errors (plus any 'extraAcceptedErrors'
    // specific to the caller) and rethrowing anything else.
    let runToleratingAcceptedErrors = function (fn, extraAcceptedErrors = []) {
        try {
            fn();
        } catch (e) {
            const acceptedErrors = [...commonAcceptedErrors, ...extraAcceptedErrors];
            if (e.code && acceptedErrors.includes(e.code)) {
                return;
            }
            throw e;
        }
    };

    let states = {
        upgrade: function (db, collName) {
            jsTestLog(`Upgrade`);
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            );
        },
        downgrade: function (db, collName) {
            jsTestLog(`Downgrade`);
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
            );
        },
        insertOne: function (db, collName) {
            const coll = db[getCollNames()[0]];
            runToleratingAcceptedErrors(() => {
                const res = assert.commandWorked(
                    coll.insertOne({
                        "op": "insertOne",
                        [metaFieldName]: rndMeta(),
                        [timeFieldName]: ISODate(),
                    }),
                );
                jsTest.log(`${coll.getName()} insertOne: ${tojsononeline(res)}`);
            });
        },
        insertMany: function (db, collName) {
            const coll = db[getCollNames()[0]];
            let docs = [];
            for (let i = 0; i < 1000; i++) {
                docs.push({
                    "op": "insertMany",
                    [metaFieldName]: rndMeta(),
                    [timeFieldName]: ISODate(),
                });
            }
            runToleratingAcceptedErrors(() => {
                const res = assert.commandWorked(coll.insertMany(docs));
                jsTest.log(`${coll.getName()} insertMany: ${tojsononeline(res)}`);
            });
        },
        deleteOne: function (db, collName) {
            const coll = db[getCollNames()[0]];
            runToleratingAcceptedErrors(() => {
                const res = assert.commandWorked(coll.deleteOne({[metaFieldName]: rndMeta()}));
                jsTest.log(`${coll.getName()} deleteOne: ${tojsononeline(res)}`);
            });
        },
        deleteMany: function (db, collName) {
            const coll = db[getCollNames()[0]];
            runToleratingAcceptedErrors(() => {
                const res = assert.commandWorked(coll.deleteMany({[metaFieldName]: rndMeta()}));
                jsTest.log(`${coll.getName()} deleteMany: ${tojsononeline(res)}`);
            });
        },
        updateMany: function (db, collName) {
            const coll = db[getCollNames()[0]];
            runToleratingAcceptedErrors(
                () => {
                    const res = assert.commandWorked(
                        coll.updateMany(
                            {[metaFieldName]: rndMeta()},
                            {$set: {[metaFieldName]: rndMeta()}},
                        ),
                    );
                    jsTest.log(`${coll.getName()} updateMany: ${tojsononeline(res)}`);
                },
                // A concurrent FCV transition can cause a partial multi-update to fail with
                // QueryPlanKilled (StaleConfig rewritten to prevent unsafe router retry).
                [ErrorCodes.QueryPlanKilled],
            );
        },
        find: function (db, collName) {
            const coll = db[getCollNames()[0]];
            runToleratingAcceptedErrors(() => {
                const res = coll.find().itcount();
                jsTest.log(`${coll.getName()} find ${res} docs`);
            });
        },
        countDocuments: function (db, collName) {
            const coll = db[getCollNames()[0]];
            runToleratingAcceptedErrors(() => {
                const res = coll.countDocuments({});
                jsTest.log(`${coll.getName()} counted ${res} docs`);
            });
        },
        aggregate: function (db, collName) {
            const coll = db[getCollNames()[0]];
            runToleratingAcceptedErrors(() => {
                const res = coll.aggregate([{"$match": {[metaFieldName]: rndMeta()}}]).toArray();
                jsTest.log(`${coll.getName()} aggregate found ${res.length}`);
            });
        },
    };

    let setup = function (db, collName) {
        const collNames = getCollNames();
        for (const collName of collNames) {
            db.createCollection(collName, {
                timeseries: {timeField: timeFieldName, metaField: metaFieldName},
            });
        }
    };

    let teardown = function (db, collName) {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
    };

    return {
        threadCount: 12,
        iterations: 300,
        startState: "upgrade",
        states: states,
        transitions: uniformDistTransitions(states),
        setup: setup,
        teardown: teardown,
    };
})();
