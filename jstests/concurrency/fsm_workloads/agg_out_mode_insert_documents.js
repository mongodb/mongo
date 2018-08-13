'use strict';

/**
 * agg_out_mode_insert_documents.js
 *
 * Tests $out with mode "insertDocuments" concurrently with moveChunk operations on the output
 * collection.
 *
 * @tags: [requires_sharding, assumes_balancer_off, assumes_autosplit_off,
 * requires_non_retryable_writes]]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                 // for extendWorkload
load('jstests/concurrency/fsm_workloads/agg_with_chunk_migrations.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    // Set the collection to run concurrent moveChunk operations as the output collection.
    $config.data.collWithMigrations = "out_mode_insert_documents";
    $config.data.threadRunCount = 0;

    $config.states.aggregate = function aggregate(db, collName, connCache) {
        const res = db[collName].aggregate([
            {
              $project: {
                  "_id.tid": {$literal: this.tid},
                  "_id.count": {$literal: this.threadRunCount},
                  "_id.doc": "$_id"
              }
            },
            {$out: {to: this.collWithMigrations, mode: "insertDocuments"}},
        ]);

        // $out should always return 0 documents.
        assert.eq(0, res.itcount());
        // If running with causal consistency, the writes may not have propagated to the secondaries
        // yet.
        assert.soon(() => {
            return this.numDocs ==
                db[this.collWithMigrations]
                    .find({"_id.tid": this.tid, "_id.count": this.threadRunCount})
                    .itcount();
        });

        this.threadRunCount += 1;
    };

    return $config;
});
