/**
 * update_where.js
 *
 * Bulk inserts documents in batches of 100, randomly selects ~1/10th of documents inserted by the
 * thread and updates them. Also queries by the thread that created the documents to verify counts.
 * @tags: [
 *  # Uses $where operator
 *  requires_scripting,
 *  requires_getmore,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_where.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.randomBound = 10;
    $config.data.generateDocumentToInsert = function generateDocumentToInsert() {
        return {tid: this.tid, x: Random.randInt(this.randomBound)};
    };

    $config.states.update = function update(db, collName) {
        var res = db[collName].update(
            // Server-side JS does not support Random.randInt, so use Math.floor/random instead
            {
                $where: 'this.x === Math.floor(Math.random() * ' + this.randomBound + ') ' +
                    '&& this.tid === ' + this.tid
            },
            {$set: {x: Random.randInt(this.randomBound)}},
            {multi: true});
        assert.commandWorked(res);

        assert.gte(res.nModified, 0);
        assert.lte(res.nModified, this.insertedDocuments);
    };

    $config.transitions = {
        insert: {insert: 0.2, update: 0.4, query: 0.4},
        update: {insert: 0.4, update: 0.2, query: 0.4},
        query: {insert: 0.4, update: 0.4, query: 0.2}
    };

    $config.setup = function setup(db, collName, cluster) {
        /* no-op to prevent index from being created */
    };

    return $config;
});
