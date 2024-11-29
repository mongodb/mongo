/**
 * upsert_where.js
 *
 * Bulk inserts documents in batches of 100, randomly selects a document that doesn't exist and
 * updates it, and queries by the thread that created the documents to verify counts.
 * @tags: [
 *   # cannot use upsert command with $where with sharded collections
 *   assumes_unsharded_collection,
 *   # Uses $where operator
 *   requires_scripting,
 *   # TODO (SERVER-91002): server side javascript execution is deprecated, and the balancer is not
 *   # compatible with it, once the incompatibility is taken care off we can re-enable this test
 *   assumes_balancer_off
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

    $config.states.upsert = function upsert(db, collName) {
        var res = db[collName].update(
            {$where: 'this.x === ' + this.randomBound + ' && this.tid === ' + this.tid},
            {$set: {x: Random.randInt(this.randomBound), tid: this.tid}},
            {upsert: true});
        assert.eq(res.nUpserted, 1);
        var upsertedDocument = db[collName].findOne({_id: res.getUpsertedId()._id});
        assert.eq(upsertedDocument.tid, this.tid);
        this.insertedDocuments += res.nUpserted;
    };

    $config.transitions = {
        insert: {insert: 0.2, upsert: 0.4, query: 0.4},
        upsert: {insert: 0.4, upsert: 0.2, query: 0.4},
        query: {insert: 0.4, upsert: 0.4, query: 0.2}
    };

    $config.setup = function setup(db, collName, cluster) {
        /* no-op to prevent index from being created */
    };

    return $config;
});
