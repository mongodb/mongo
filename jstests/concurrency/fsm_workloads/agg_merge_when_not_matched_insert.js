'use strict';

/**
 * agg_merge_when_not_matched_insert.js
 *
 * Tests $merge with "whenNotMatched" set to "insert" concurrently with moveChunk operations on the
 * output collection.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  incompatible_with_gcov,
 *]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                 // for extendWorkload
load('jstests/concurrency/fsm_workloads/agg_with_chunk_migrations.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    // Set the collection to run concurrent moveChunk operations as the output collection.
    $config.data.collWithMigrations = "agg_merge_when_not_matched_insert";
    $config.data.threadRunCount = 0;

    let initialMaxCatchUpPercentageBeforeBlockingWrites = null;

    $config.states.aggregate = function aggregate(db, collName, connCache) {
        const res = db[collName].aggregate([
            {
                $project: {
                    "_id.tid": {$literal: this.tid},
                    "_id.count": {$literal: this.threadRunCount},
                    "_id.doc": "$_id"
                }
            },
            {
                $merge:
                    {into: this.collWithMigrations, whenMatched: "fail", whenNotMatched: "insert"}
            },
        ]);

        // $merge should always return 0 documents.
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

    // This test is sensitive to low values of the parameter
    // maxCatchUpPercentageBeforeBlockingWrites, which can be set by the config server. We set a min
    // bound for this parameter here.
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, [db, collName, cluster]);

        cluster.executeOnMongodNodes((db) => {
            const param = assert.commandWorked(
                db.adminCommand({getParameter: 1, maxCatchUpPercentageBeforeBlockingWrites: 1}));
            if (param.hasOwnProperty("maxCatchUpPercentageBeforeBlockingWrites")) {
                const defaultValue = 10;
                if (param.maxCatchUpPercentageBeforeBlockingWrites < defaultValue) {
                    jsTest.log(
                        "Parameter `maxCatchUpPercentageBeforeBlockingWrites` value too low: " +
                        param.maxCatchUpPercentageBeforeBlockingWrites +
                        ". Setting value to default: " + defaultValue + ".");
                    initialMaxCatchUpPercentageBeforeBlockingWrites =
                        param.maxCatchUpPercentageBeforeBlockingWrites;
                    assert.commandWorked(db.adminCommand(
                        {setParameter: 1, maxCatchUpPercentageBeforeBlockingWrites: defaultValue}));
                }
            }
        });
    };

    $config.teardown = function teardown(db, collName, cluster) {
        if (initialMaxCatchUpPercentageBeforeBlockingWrites) {
            jsTest.log(
                "Resetting parameter `maxCatchUpPercentageBeforeBlockingWrites` to original value: " +
                initialMaxCatchUpPercentageBeforeBlockingWrites);
            cluster.executeOnMongodNodes((db) => {
                assert.commandWorked(db.adminCommand({
                    setParameter: 1,
                    maxCatchUpPercentageBeforeBlockingWrites:
                        initialMaxCatchUpPercentageBeforeBlockingWrites
                }));
            });
        }

        $super.teardown.apply(this, [db, collName, cluster]);
    };

    return $config;
});
