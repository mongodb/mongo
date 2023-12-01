/**
 * remove_where.js
 *
 * Bulk inserts documents in batches of 100. Randomly selects ~1/10th of existing documents created
 * by the thread and removes them. Queries by the thread that created the documents to verify
 * counts.
 *
 * When the balancer is enabled, the nRemoved result may be inaccurate as
 * a chunk migration may be active, causing the count function to assert.
 *
 * @tags: [
 *  assumes_balancer_off,
 *  # Uses $where operator
 *  requires_scripting,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/indexed_insert_where.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
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
        assert.gte(res.nRemoved, 0);
        assert.lte(res.nRemoved, this.insertedDocuments);
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

    return $config;
});
