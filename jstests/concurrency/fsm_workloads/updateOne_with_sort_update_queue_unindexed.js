/**
 * updateOne_update_queue_unindexed.js
 *
 * This is the same workload as updateOne_update_queue.js, but without the relevant index.
 *
 * The lack of an index that could satisfy the sort forces the updateOne operations to scan all
 * the matching documents in order to find the relevant document. This increases the amount of work
 * each updateOne operation has to do before getting to the matching document, and thus
 * increases the chance of a write conflict because each concurrent updateOne operation is
 * trying to update the same document from the queue.
 *
 * This test is modeled off of findAndModify_update_queue.js, but instead of storing the _id field
 * of the updated document in another database and ensuring that every thread updated a different
 * document from the other threads, we check that the correct number of documents were updated
 * because updateOne doesn't return the modified document (and its _id value) unless upsert is true.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1, resolving the issue about assumes_unsharded_collection.
 *   requires_fcv_80,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/updateOne_with_sort_update_queue.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Use the same workload name as the database name, since the workload
    // name is assumed to be unique.
    $config.data.uniqueDBName = jsTestName();

    $config.data.getIndexSpecs = function getIndexSpecs() {
        return [];
    };

    return $config;
});
