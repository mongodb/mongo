/**
 * findAndModify_update_queue_unindexed.js
 *
 * This is the same workload as findAndModify_update_queue.js, but without the relevant index.
 *
 * The lack of an index that could satisfy the sort forces the findAndModify operations to scan all
 * the matching documents in order to find the relevant document. This increases the amount of work
 * each findAndModify operation has to do before getting to the matching document, and thus
 * increases the chance of a write conflict because each concurrent findAndModify operation is
 * trying to update the same document from the queue.
 *
 * This workload was designed to reproduce SERVER-21434.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1.
 *   requires_fcv_71,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/findAndModify/findAndModify_update_queue.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Use the workload name as the database name, since the workload
    // name is assumed to be unique.
    $config.data.uniqueDBName = jsTestName();

    $config.data.getIndexSpecs = function getIndexSpecs() {
        return [];
    };

    return $config;
});
