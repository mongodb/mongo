'use strict';

/**
 * Test a snapshot read spanning a find and getmore that runs concurrently with
 * killOp and txnNumber change.

 * TODO: SERVER-35567 - Delete this workload.

 * @tags: [uses_transactions, state_functions_share_transaction]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');                     // for extendWorkload
load('jstests/concurrency/fsm_workloads/snapshot_read_kill_operations.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.transitions = {
        init: {snapshotFind: 1.0},
        snapshotFind: {incrementTxnNumber: 0.33, killOp: 0.34, snapshotGetMore: 0.33},
        incrementTxnNumber: {snapshotGetMore: 1.0},
        killOp: {snapshotGetMore: 1.0},
        snapshotGetMore: {snapshotFind: 1.0}
    };

    return $config;
});
