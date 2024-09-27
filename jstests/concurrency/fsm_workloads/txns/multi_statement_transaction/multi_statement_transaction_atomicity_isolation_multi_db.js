/**
 * Test transactions atomicity and isolation guarantees for transactions across multiple DBs.
 *
 * @tags: [uses_transactions, assumes_snapshot_transactions]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/txns/multi_statement_transaction/multi_statement_transaction_atomicity_isolation.js";

export const $config = extendWorkload($baseConfig, ($config, $super) => {
    // Number of unique collections and number of unique databases. The square root is used
    // here to ensure the total number of namespaces (coll * db) is roughly equal to the
    // number of threads.
    const nsCount = Math.max(2, Math.floor(Math.sqrt($config.threadCount)));

    $config.data.getAllCollections = (db, collName) => {
        const collections = [];
        for (let i = 0; i < nsCount; ++i) {
            for (let j = 0; j < nsCount; ++j) {
                collections.push(
                    db.getSiblingDB(db.getName() + '_' + i).getCollection(collName + '_' + j));
            }
        }
        return collections;
    };

    return $config;
});
