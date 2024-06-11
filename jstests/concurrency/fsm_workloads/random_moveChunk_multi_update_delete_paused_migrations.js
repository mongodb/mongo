'use strict';

/**
 * This test performs random update/delete batches while chunk migrations are ongoing in a
 * collection sharded on {skey: 1}, an integer field.
 *
 * Updates always increment the counter field of the targeted documents by 1, but otherwise randomly
 * select values for multi and upsert:
 *
 * multi: false, upsert: false -> Targets by skey and updates a single owned document.
 *
 * multi: true, upsert: false -> Targets by thread id and updates all owned documents.
 *
 * multi: false, upsert: true -> Reinsert an owned document previously deleted by a limit: 1
 * delete if possible, otherwise behaves the same as multi:false, upsert: false.
 *
 * multi: true, upsert: true -> Same as multi: false, upsert: true because the server requires
 * upserts target by shard key.
 *
 * Deletes randomly vary by limit:
 *
 * limit: 0 -> Targets by thread id and deletes all owned documents. After all deletes in a batch
 * containing a limit: 0 delete are complete, all documents owned by this thread will be reinserted
 * according to their initial state (i.e. counter 0).
 *
 * limit: 1 -> Targets by skey and deletes a single owned document. Documents deleted in this
 * manner will be prioritized by future upserts.
 *
 * @tags: [
 * requires_sharding,
 * assumes_balancer_off,
 * incompatible_with_concurrency_simultaneous,
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

function chooseRandomMapValue(map) {
    const keys = Array.from(map.keys());
    const index = Random.randInt(keys.length);
    const doc = map.get(keys[index]);
    return doc;
}

function getPauseMigrationsClusterParameter(db) {
    const response = assert.commandWorked(
        db.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));
    return response.clusterParameters[0].enabled;
}

function setPauseMigrationsClusterParameter(db, cluster, enabled) {
    assert.commandWorked(
        db.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled}}}));

    cluster.executeOnMongosNodes((db) => {
        // Ensure all mongoses have refreshed cluster parameter after being set.
        assert.soon(() => {
            return getPauseMigrationsClusterParameter(db) === enabled;
        });
    });
}

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;
    $config.data.partitionSize = 100;

    $config.data.isMoveChunkErrorAcceptable = (err) => {
        return err.code === ErrorCodes.Interrupted;
    };

    $config.data.getTargetedDocuments = function getTargetedDocuments(collName, query) {
        const shardKeyField = this.shardKeyField[collName];
        const keys = new Set();
        if (shardKeyField in query) {
            keys.add(query[shardKeyField]);
        } else {
            for (const [key, _] of this.expectedDocs) {
                keys.add(key);
            }
        }
        return keys;
    };

    $config.data.incrementCounterForDoc = function incrementCounterForDoc(key) {
        const original = (() => {
            if (this.expectedDocs.has(key)) {
                return this.expectedDocs.get(key);
            }
            return this.initialDocs.get(key);
        })();
        this.expectedDocs.set(key, {...original, counter: original.counter + 1});
    };

    $config.data.createRandomUpdateBatch = function createRandomUpdateBatch(collName) {
        const batchSize = 1 + Random.randInt(2);
        const updates = [];
        for (let i = 0; i < batchSize; i++) {
            updates.push(this.createRandomUpdate(collName));
        }
        return updates;
    };

    $config.data.createRandomUpdate = function createRandomUpdate(collName) {
        const multi = Random.rand() > 0.5;
        const upsert = Random.rand() > 0.5;
        const q = this.createUpdateQuery(collName, multi, upsert);
        return {q, u: {$inc: {counter: 1}}, multi, upsert};
    };

    $config.data.createUpdateQuery = function createUpdateQuery(collName, multi, upsert) {
        let query = {tid: this.tid};
        if (multi && !upsert) {
            return query;
        }
        // If we are an updateOne or upsert, we need to choose a specific document to update.
        const documents = (() => {
            if (upsert) {
                // Prefer to reinsert a previously deleted document if possible.
                const deleted = this.getDeletedDocuments();
                if (deleted.size > 0) {
                    return deleted;
                }
            }
            return this.expectedDocs;
        })();
        const shardKeyField = this.shardKeyField[collName];
        query[shardKeyField] = chooseRandomMapValue(documents)[shardKeyField];
        return query;
    };

    $config.data.createRandomDeleteBatch = function createRandomDeleteBatch(collName) {
        const batchSize = 1 + Random.randInt(2);
        const deletes = [];
        for (let i = 0; i < batchSize; i++) {
            deletes.push(this.createRandomDelete(collName));
        }
        return deletes;
    };

    $config.data.createRandomDelete = function createRandomDelete(collName) {
        const limit = Random.rand() > 0.5 ? 0 : 1;
        return {q: this.createDeleteQuery(collName, limit), limit};
    };

    $config.data.createDeleteQuery = function createDeleteQuery(collName, limit) {
        let query = {tid: this.tid};
        if (limit === 0) {
            return query;
        }
        if (this.expectedDocs.size === 0) {
            return query;
        }
        // If we are a deleteOne, we need to choose a specific document to delete.
        const shardKeyField = this.shardKeyField[collName];
        query[shardKeyField] = chooseRandomMapValue(this.expectedDocs)[shardKeyField];
        return query;
    };

    $config.data.getDeletedDocuments = function getDeletedDocuments() {
        const deleted = new Map();
        for (const [key, doc] of this.initialDocs) {
            if (!this.expectedDocs.has(key)) {
                deleted.set(key, doc);
            }
        }
        return deleted;
    };

    $config.data.readOwnedDocuments = function readOwnedDocuments(db, collName) {
        const shardKeyField = this.shardKeyField[collName];
        const documents = findFirstBatch(db, collName, {tid: this.tid}, 1000);
        const map = new Map();
        for (const doc of documents) {
            map.set(doc[shardKeyField], doc);
        }
        return map;
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        setPauseMigrationsClusterParameter(db, cluster, true);
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, arguments);
        assert(migrationsAreAllowed(db, collName));
        setPauseMigrationsClusterParameter(db, cluster, false);
    };

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);

        findFirstBatch(db, collName, {tid: this.tid}, 1000).forEach(doc => {
            db[collName].update({_id: doc._id}, {$set: {counter: 0}});
        });

        this.initialDocs = this.readOwnedDocuments(db, collName);
        this.expectedDocs = new Map(this.initialDocs);
        jsTestLog(`Thread with tid ${this.tid} owns ${this.initialDocs.size} documents`);
    };

    $config.states.multiUpdate = function multiUpdate(db, collName, connCache) {
        ignoreErrorsIfInNonTransactionalStepdownSuite(() => {
            const updates = this.createRandomUpdateBatch(collName);
            jsTestLog("Executing updates: " + tojson(updates));
            const result = db.runCommand({update: collName, updates});
            jsTestLog("Result: " + tojson(result));
            assert.commandWorked(result);
            let totalUpdates = 0;
            let totalUpserts = 0;
            for (const update of updates) {
                const updatedKeys = this.getTargetedDocuments(collName, update.q);
                totalUpdates += updatedKeys.size;
                for (const key of updatedKeys) {
                    if (!this.expectedDocs.has(key)) {
                        totalUpserts++;
                    }
                    this.incrementCounterForDoc(key);
                }
            }
            assert.eq(totalUpdates, result.n);
            assert.eq(totalUpdates - totalUpserts, result.nModified);
        });
    };

    $config.states.multiDelete = function multiDelete(db, collName, connCache) {
        ignoreErrorsIfInNonTransactionalStepdownSuite(() => {
            const deletes = this.createRandomDeleteBatch(collName);
            jsTestLog("Executing deletes: " + tojson(deletes));
            const result = db.runCommand({delete: collName, deletes});
            jsTestLog("Result: " + tojson(result));
            assert.commandWorked(result);
            let uniqueDeletes = new Set();
            for (const deleteOp of deletes) {
                const deletedKeys = this.getTargetedDocuments(collName, deleteOp.q);
                for (const key of deletedKeys) {
                    uniqueDeletes.add(key);
                    this.expectedDocs.delete(key);
                }
            }
            assert.eq(uniqueDeletes.size, result.n);

            if (this.expectedDocs.size > 0) {
                return;
            }
            jsTestLog(`Thread ${this.tid} has deleted all of its documents and will now reset to its initial state`);
            const bulk = db[collName].initializeUnorderedBulkOp();
            for (const [_, doc] of this.initialDocs) {
                bulk.insert(doc);
            }
            assert.commandWorked(bulk.execute());
            this.expectedDocs = new Map(this.initialDocs);
        });
    };

    $config.states.verify = function verify(db, collName, connCache) {
        ignoreErrorsIfInNonTransactionalStepdownSuite(() => {
            assert.eq(this.expectedDocs, this.readOwnedDocuments(db, collName));
        });
    };

    const weights = {moveChunk: 0.2, multiUpdate: 0.35, multiDelete: 0.35, verify: 0.1};
    $config.transitions = {
        init: weights,
        moveChunk: weights,
        multiUpdate: weights,
        multiDelete: weights,
        verify: weights,
    };

    return $config;
});
