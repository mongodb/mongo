/**
 * Tests periodically killing sessions that are running transactions that may use the single write
 * shard commit optimization.
 *
 * TODO (SERVER-88903): Investigate why multi_statement_transaction_simple_read_write_kill_
 * sessions.js hang when run in concurrency_embedded_router_* suites on config shard build variant.
 * @tags: [
 *    uses_transactions,
 *    assumes_snapshot_transactions,
 *    kills_random_sessions,
 *    config_shard_incompatible
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {killSession} from "jstests/concurrency/fsm_workload_helpers/kill_session.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/multi_statement_transaction_simple_read_write.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.retryOnKilledSession = true;

    $config.states.killSession = killSession;

    $config.transitions = {
        init: {transferMoney: 1},
        transferMoney: {
            transferMoney: 0.1,
            checkMoneyBalance: 0.2,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
            killSession: 0.1,
        },
        checkMoneyBalance: {
            transferMoney: 0.3,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
            killSession: 0.1,
        },
        writeThenReadTxn: {
            transferMoney: 0.1,
            checkMoneyBalance: 0.2,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
            killSession: 0.1,
        },
        readThenWriteTxn: {
            transferMoney: 0.1,
            checkMoneyBalance: 0.2,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
            killSession: 0.1,
        },
        readThenWriteThenReadTxn: {
            transferMoney: 0.1,
            checkMoneyBalance: 0.2,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
            killSession: 0.1,
        },
        killSession: {
            transferMoney: 0.2,
            checkMoneyBalance: 0.2,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
            killSession: 0.1,
        },
    };

    return $config;
});
