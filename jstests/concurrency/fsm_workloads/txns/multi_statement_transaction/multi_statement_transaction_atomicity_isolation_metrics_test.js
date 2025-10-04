/**
 * This test checks high level invariants of various transaction related metrics reported in
 * serverStatus and currentOp.
 *
 * @tags: [
 *  uses_transactions,
 *  uses_prepare_transaction,
 *  assumes_snapshot_transactions,
 *  # serverStatus does not include transaction metrics on mongos.
 *  assumes_against_mongod_not_mongos,
 *  # This test runs slow on TSAN builds and might trigger evergreen timeout.
 *  tsan_incompatible,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {checkServerStatusInvariants} from "jstests/concurrency/fsm_workload_helpers/check_transaction_server_status_invariants.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/txns/multi_statement_transaction/multi_statement_transaction_atomicity_isolation.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.setup = function (db, collName, cluster) {
        $super.setup.apply(this, arguments);
        this.prepareProbability = 0.5;
    };

    $config.teardown = function (db, collName, cluster) {
        // Check the server-wide invariants one last time, with only a single sample, since all user
        // operations should have finished.
        checkServerStatusInvariants(db, 1, false /* isMongos */);
        $super.teardown.apply(this, arguments);
    };

    $config.states.checkInvariants = function checkInvariants(db, collName) {
        // Check server-wide invariants using 100 samples. This sample size is deemed big enough to
        // account for transient inconsistencies, which we assume are rare.
        let nSamples = 100;
        checkServerStatusInvariants(db, nSamples, false /* isMongos */);

        // Check currentOp metrics invariants for all running transactions. These timing related
        // invariants are expected to always hold.
        let currentOp = db.currentOp({"transaction": {$exists: true}});
        currentOp.inprog.forEach((op) => {
            let txnStats = op.transaction;
            let timeActive = Number(txnStats["timeActiveMicros"]);
            let timeInactive = Number(txnStats["timeInactiveMicros"]);
            let timeOpen = Number(txnStats["timeOpenMicros"]);
            assert.eq(timeActive + timeInactive, timeOpen, () => tojson(txnStats));
        });
    };

    $config.transitions = {
        init: {update: 0.9, checkInvariants: 0.1},
        update: {update: 0.8, checkInvariants: 0.1, causalRead: 0.1},
        checkInvariants: {update: 0.9, causalRead: 0.1},
        causalRead: {update: 1.0},
    };

    return $config;
});
