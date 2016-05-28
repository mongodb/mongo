'use strict';

/**
 * upsert_where.js
 *
 * Bulk inserts documents in batches of 100, randomly selects a document that doesn't exist and
 * updates it, and queries by the thread that created the documents to verify counts. */

load('jstests/concurrency/fsm_libs/extend_workload.js');            // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_where.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.randomBound = 10;
    $config.data.generateDocumentToInsert = function generateDocumentToInsert() {
        return {tid: this.tid, x: Random.randInt(this.randomBound)};
    };

    $config.states.upsert = function upsert(db, collName) {
        var res = db[collName].update(
            {$where: 'this.x === ' + this.randomBound + ' && this.tid === ' + this.tid},
            {$set: {x: Random.randInt(this.randomBound), tid: this.tid}},
            {upsert: true});
        assertWhenOwnColl.eq(res.nUpserted, 1);
        var upsertedDocument = db[collName].findOne({_id: res.getUpsertedId()._id});
        assertWhenOwnColl(function() {
            assertWhenOwnColl.eq(upsertedDocument.tid, this.tid);
        }.bind(this));
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
