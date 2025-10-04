/**
 * collmod.js
 *
 * Base workload for collMod command.
 * Generates some random data and inserts it into a collection with a
 * TTL index. Runs a collMod command to change the value of the
 * expireAfterSeconds setting to a random integer.
 *
 * All threads update the same TTL index on the same collection.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export const $config = (function () {
    let data = {
        numDocs: 1000,
        maxTTL: 5000, // max time to live
    };

    let states = (function () {
        function collMod(db, collName) {
            let newTTL = Random.randInt(this.maxTTL);
            let res = db.runCommand({
                collMod: this.threadCollName,
                index: {keyPattern: {createdAt: 1}, expireAfterSeconds: newTTL},
            });
            assert.commandWorkedOrFailedWithCode(res, [ErrorCodes.ConflictingOperationInProgress]);
            // Check that we are returning {ok: 1} rather than {ok: true}
            if (res.ok) {
                assert(res.ok === 1);
            }
            // only assert if new expireAfterSeconds differs from old one
            if (res.ok === 1 && res.hasOwnProperty("expireAfterSeconds_new")) {
                assert.eq(res.expireAfterSeconds_new, newTTL);
            }

            // Attempt an invalid collMod which should always fail regardless of whether a WCE
            // occurred. This is meant to reproduce SERVER-56772.
            const encryptSchema = {$jsonSchema: {properties: {_id: {encrypt: {}}}}};
            res = assert.commandFailedWithCode(
                db.runCommand({
                    collMod: this.threadCollName,
                    validator: encryptSchema,
                    validationAction: "warn",
                }),
                [ErrorCodes.ConflictingOperationInProgress, ErrorCodes.QueryFeatureNotAllowed],
            );
            // Check that we are returning {ok: 1} rather than {ok: true}
            if (res.ok) {
                assert(res.ok === 1);
            }
            if (FeatureFlagUtil.isPresentAndEnabled(db, "ErrorAndLogValidationAction")) {
                res = assert.commandFailedWithCode(
                    db.runCommand({
                        collMod: this.threadCollName,
                        validator: encryptSchema,
                        validationAction: "errorAndLog",
                    }),
                    [ErrorCodes.ConflictingOperationInProgress, ErrorCodes.QueryFeatureNotAllowed],
                );
                // Check that we are returning {ok: 1} rather than {ok: true}
                if (res.ok) {
                    assert(res.ok === 1);
                }
            }
        }

        return {collMod: collMod};
    })();

    let transitions = {collMod: {collMod: 1}};

    function setup(db, collName, cluster) {
        // other workloads that extend this one might have set 'this.threadCollName'
        this.threadCollName = this.threadCollName || collName;
        let bulk = db[this.threadCollName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            bulk.insert({createdAt: new Date()});
        }

        let res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);

        // create TTL index
        res = db[this.threadCollName].createIndex({createdAt: 1}, {expireAfterSeconds: 3600});
        assert.commandWorked(res);
    }

    return {
        threadCount: 10,
        iterations: 20,
        data: data,
        startState: "collMod",
        states: states,
        transitions: transitions,
        setup: setup,
    };
})();
