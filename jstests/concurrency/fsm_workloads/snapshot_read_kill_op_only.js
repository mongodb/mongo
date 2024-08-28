/**
 * Test a snapshot read spanning a find and getmore that runs concurrently with
 * killOp and txnNumber change.

 * TODO: SERVER-39939 - Delete this workload.

 * @tags: [uses_transactions, state_functions_share_transaction, requires_getmore]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/snapshot_read_kill_operations.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.transitions = {
        init: {snapshotFind: 1.0},
        snapshotFind: {incrementTxnNumber: 0.33, killOp: 0.34, snapshotGetMore: 0.33},
        incrementTxnNumber: {snapshotGetMore: 1.0},
        killOp: {snapshotGetMore: 1.0},
        snapshotGetMore: {snapshotFind: 1.0}
    };

    return $config;
});
