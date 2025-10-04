/**
 * Tests periodically killing sessions that are running transactions. The base workload runs
 * transactions with two writes, which will require two phase commit in a sharded cluster if each
 * write targets a different shard.
 *
 * @tags: [uses_transactions, assumes_snapshot_transactions, kills_random_sessions]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {killSession} from "jstests/concurrency/fsm_workload_helpers/kill_session.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/txns/multi_statement_transaction/multi_statement_transaction_simple.js";

export const $config = extendWorkload($baseConfig, ($config, $super) => {
    $config.data.retryOnKilledSession = true;

    $config.states.killSession = killSession;

    $config.transitions = {
        init: {transferMoney: 1},
        transferMoney: {transferMoney: 0.8, checkMoneyBalance: 0.1, killSession: 0.1},
        checkMoneyBalance: {transferMoney: 0.9, killSession: 0.1},
        killSession: {transferMoney: 0.8, checkMoneyBalance: 0.1, killSession: 0.1},
    };

    return $config;
});
