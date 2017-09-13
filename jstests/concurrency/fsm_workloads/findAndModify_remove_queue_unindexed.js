'use strict';

/**
 * findAndModify_remove_queue_unindexed.js
 *
 * This is the same workload as findAndModify_remove_queue.js, but without the relevant index.
 *
 * The lack of an index that could satisfy the sort forces the findAndModify operations to scan all
 * the matching documents in order to find the relevant document. This increases the amount of work
 * each findAndModify operation has to do before getting to the matching document, and thus
 * increases the chance of a write conflict because each concurrent findAndModify operation is
 * trying to remove the same document from the queue.
 *
 * This workload was designed to reproduce SERVER-21434.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                  // for extendWorkload
load('jstests/concurrency/fsm_workloads/findAndModify_remove_queue.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    // Use the workload name as the database name, since the workload
    // name is assumed to be unique.
    $config.data.uniqueDBName = 'findAndModify_remove_queue_unindexed';

    $config.data.getIndexSpecs = function getIndexSpecs() {
        return [];
    };

    return $config;
});
