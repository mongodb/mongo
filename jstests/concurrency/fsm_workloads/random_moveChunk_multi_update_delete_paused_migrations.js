'use strict';

/**
 * Performs a series of {multi: true} updates/deletes while moving chunks with
 * pauseMigrationsDuringMultiUpdates enabled and verifies no updates or deletes
 * are lost.
 *
 * @tags: [
 * requires_sharding,
 * assumes_balancer_off,
 * featureFlagPauseMigrationsDuringMultiUpdatesAvailable,
 * requires_fcv_80
 * ];
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/random_moveChunk_base.js";
import {
    findFirstBatch,
    withSkipRetryOnNetworkError
} from "jstests/concurrency/fsm_workload_helpers/stepdown_suite_helpers.js";
import {migrationsAreAllowed} from "jstests/libs/chunk_manipulation_util.js";

function ignoreErrorsIfInNonTransactionalStepdownSuite(fn) {
    // Even while pauseMigrationsDuringMultiUpdates is enabled, updateMany and deleteMany cannot be
    // resumed after a failover, and therefore may have only partially completed (unless we were
    // running in a transaction). We can't verify any constraints related to the updates actually
    // being made, but this test is still interesting to verify that the migration blocking state is
    // correctly managed even in the presence of failovers.
    if (TestData.runningWithShardStepdowns && !TestData.runInsideTransaction) {
        try {
            withSkipRetryOnNetworkError(fn);
        } catch (e) {
            jsTest.log("Ignoring error: " + e.code);
        }
    } else {
        fn();
    }
}

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;
    $config.data.partitionSize = 100;

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        assert.commandWorked(db.adminCommand(
            {setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}}}));
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, arguments);
        assert(migrationsAreAllowed(db, collName));
    };

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);

        this.expectedCount = 0;
        findFirstBatch(db, collName, {tid: this.tid}, 1000).forEach(doc => {
            db[collName].update({_id: doc._id}, {$set: {counter: this.expectedCount}});
        });

        this.initialDocs = findFirstBatch(db, collName, {tid: this.tid}, 1000);
    };

    $config.states.multiUpdate = function multiUpdate(db, collName, connCache) {
        ignoreErrorsIfInNonTransactionalStepdownSuite(() => {
            const result = db.runCommand({
                update: collName,
                updates: [{q: {tid: this.tid}, u: {$inc: {counter: 1}}, multi: true}]
            })
            assert.commandWorked(result);
            assert.eq(result.n, this.initialDocs.length);
            assert.eq(result.n, result.nModified);
            this.expectedCount++;
        });
    };

    $config.states.multiDelete = function multiDelete(db, collName, connCache) {
        ignoreErrorsIfInNonTransactionalStepdownSuite(() => {
            const result =
                db.runCommand({delete: collName, deletes: [{q: {tid: this.tid}, limit: 0}]});
            assert.commandWorked(result);
            assert.eq(result.n, this.initialDocs.length);

            const bulk = db[collName].initializeUnorderedBulkOp();
            for (const doc of this.initialDocs) {
                bulk.insert(doc);
            }
            assert.commandWorked(bulk.execute());
            this.expectedCount = 0;
        });
    };

    $config.states.verify = function verify(db, collName, connCache) {
        ignoreErrorsIfInNonTransactionalStepdownSuite(() => {
            db[collName].find({tid: this.tid}).forEach(doc => {
                assert.eq(doc.counter, this.expectedCount);
            });
        });
    };

    // TODO SERVER-85866: Set moveChunk weight to 0.2.
    const weights = {moveChunk: 0.0, multiUpdate: 0.35, multiDelete: 0.35, verify: 0.1};
    $config.transitions = {
        init: weights,
        moveChunk: weights,
        multiUpdate: weights,
        multiDelete: weights,
        verify: weights,
    };

    return $config;
});
