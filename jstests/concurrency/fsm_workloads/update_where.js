'use strict';

/**
 * update_where.js
 *
 * Bulk inserts documents in batches of 100, randomly selects ~1/10th of documents inserted by the
 * thread and updates them. Also queries by the thread that created the documents to verify counts.
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');            // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_where.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
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
        assertAlways.writeOK(res);

        if (db.getMongo().writeMode() === 'commands') {
            assertWhenOwnColl.gte(res.nModified, 0);
            assertWhenOwnColl.lte(res.nModified, this.insertedDocuments);
        }
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
