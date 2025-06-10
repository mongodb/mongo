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
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/agg/agg_with_chunk_migrations.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Set the collection to run concurrent moveChunk operations as the output collection.
    $config.data.collWithMigrations = "agg_merge_when_not_matched_insert";
    $config.data.threadRunCount = 0;

    let initialMaxCatchUpPercentageBeforeBlockingWrites = null;

    $config.states.aggregate = function aggregate(db, collName, connCache) {
        const res = db[collName].aggregate([
            {
                $project: {
                    // The code below originally wrote `this.tid`, `this.threadRunCount`, and `$_id`
                    // into an object and used this as _id field. However, using an object as id led
                    // to poor performance when sharding (see SERVER-94976). Therefore, we create
                    // a numerical _id by combining these components to a single number. We use the
                    // formula below to have a stable _id without collisions with existing documents
                    // using ids 0..99.
                    "_id": {
                        $add: [
                            100,
                            {$multiply: [this.tid, 100_000_000]},
                            {$multiply: [this.threadRunCount, 1_000_000]},
                            {$mod: ["$_id", 1_000_000]}
                        ]
                    },
                    "meta.tid": {$literal: this.tid},
                    "meta.count": {$literal: this.threadRunCount},
                    "meta.doc": "$_id"
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
            let count;
            try {
                count = db[this.collWithMigrations]
                            .find({"meta.tid": this.tid, "meta.count": this.threadRunCount})
                            .itcount();
            } catch (e) {
                if (e.code != ErrorCodes.QueryPlanKilled) {
                    // When this query is run on a secondary, it might be killed with
                    // ErrorCodes::QueryPlanKilled due to an ongoing range deletion.
                    throw e;
                }
                return false;
            }
            return this.numDocs == count;
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
