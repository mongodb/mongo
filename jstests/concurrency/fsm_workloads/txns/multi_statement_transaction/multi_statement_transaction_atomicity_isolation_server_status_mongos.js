/**
 * Verifies the transactions server status metrics on mongos while running transactions.
 * Temporarily disabled for BF-24311.
 *
 * @tags: [__TEMPORARILY_DISABLED__, requires_sharding, assumes_snapshot_transactions,
 * uses_transactions]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {checkServerStatusInvariants} from "jstests/concurrency/fsm_workload_helpers/check_transaction_server_status_invariants.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/txns/multi_statement_transaction/multi_statement_transaction_atomicity_isolation.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.teardown = function (db, collName, cluster) {
        // Check the server-wide invariants one last time with only a single sample, since all user
        // operations should have finished.
        checkServerStatusInvariants(db, 1, true /* isMongos */);
        $super.teardown.apply(this, arguments);
    };

    $config.states.verifyServerStatus = function verifyServerStatus(db, collName) {
        const nSamples = 100;
        checkServerStatusInvariants(db, nSamples, true /* isMongos */);
    };

    $config.transitions = {
        init: {update: 0.8, checkConsistency: 0.1, verifyServerStatus: 0.1},
        update: {update: 0.7, checkConsistency: 0.1, causalRead: 0.1, verifyServerStatus: 0.1},
        checkConsistency: {update: 0.9, verifyServerStatus: 0.1},
        causalRead: {update: 0.9, verifyServerStatus: 0.1},
        verifyServerStatus: {update: 0.8, checkConsistency: 0.1, causalRead: 0.1},
    };

    return $config;
});
