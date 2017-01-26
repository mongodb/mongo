'use strict';

/**
 * remove_where.js
 *
 * Bulk inserts documents in batches of 100. Randomly selects ~1/10th of existing documents created
 * by the thread and removes them. Queries by the thread that created the documents to verify
 * counts.
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');            // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_where.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.randomBound = 10;
    $config.data.generateDocumentToInsert = function generateDocumentToInsert() {
        return {tid: this.tid, x: Random.randInt(this.randomBound)};
    };

    $config.states.remove = function remove(db, collName) {
        var res = db[collName].remove({
            // Server-side JS does not support Random.randInt, so use Math.floor/random instead
            $where: 'this.x === Math.floor(Math.random() * ' + this.randomBound + ') ' +
                '&& this.tid === ' + this.tid
        });
        assertWhenOwnColl.gte(res.nRemoved, 0);
        assertWhenOwnColl.lte(res.nRemoved, this.insertedDocuments);
        this.insertedDocuments -= res.nRemoved;
    };

    $config.transitions = {
        insert: {insert: 0.2, remove: 0.4, query: 0.4},
        remove: {insert: 0.4, remove: 0.2, query: 0.4},
        query: {insert: 0.4, remove: 0.4, query: 0.2}
    };

    $config.setup = function setup(db, collName, cluster) {
        /* no-op to prevent index from being created */
    };

    $config.skip = function skip(cluster) {
        // When the balancer is enabled, the nRemoved result may be inaccurate as
        // a chunk migration may be active, causing the count function to assert.
        if (cluster.isBalancerEnabled()) {
            return {skip: true, msg: 'does not run when balancer is enabled.'};
        }
        return {skip: false};
    };

    return $config;
});
