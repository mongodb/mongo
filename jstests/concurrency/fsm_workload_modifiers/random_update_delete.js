/**
 * This file adds stages to perform random update/delete batches.
 *
 * Updates always increment the counter field of the targeted documents by 1, but otherwise randomly
 * select values for multi and upsert:
 *
 * multi: false, upsert: false -> Targets by _id or shard key and updates a single owned document.
 *
 * multi: true, upsert: false -> Targets by thread id and updates all owned documents.
 *
 * multi: false, upsert: true -> Reinsert an owned document previously deleted by a limit: 1
 * delete if possible, otherwise behaves the same as multi:false, upsert: false.
 *
 * multi: true, upsert: true -> Same as multi: false, upsert: true because the server requires
 * upserts target by _id or shard key.
 *
 * Deletes randomly vary by limit:
 *
 * limit: 0 -> Targets by thread id and deletes all owned documents. After all deletes in a batch
 * containing a limit: 0 delete are complete, all documents owned by this thread will be reinserted
 * according to their initial state (i.e. counter 0).
 *
 * limit: 1 -> Targets by _id or shard key and deletes a single owned document. Documents deleted in
 * this manner will be prioritized by future upserts.
 */

import {
    findFirstBatch,
    inNonTransactionalStepdownSuite,
    runWithManualRetriesIfInNonTransactionalStepdownSuite
} from "jstests/concurrency/fsm_workload_helpers/stepdown_suite_helpers.js";

function chooseRandomMapValue(map) {
    const keys = Array.from(map.keys());
    const index = Random.randInt(keys.length);
    const doc = map.get(keys[index]);
    return doc;
}

export function randomUpdateDelete($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    $config.data.expectPartialMultiWrites = inNonTransactionalStepdownSuite();
    $config.data.expectExtraMultiWrites = false;
    $config.data.maxWriteBatchSize = 2;

    $config.data.getShardKey = function getShardKey(collName) {
        // If the final workload is operating on a sharded collection and doesn't always use the
        // shardKey field as its shard key, override this function.
        if (this.shardKey !== undefined) {
            return this.shardKey;
        }
        // Unsharded (i.e. unsplittable) collections always use {_id: 1} as the shard key.
        return {_id: 1};
    };

    $config.data.getShardKeyField = function getShardKeyField(collName) {
        const shardKey = this.getShardKey(collName);
        const fields = Object.keys(shardKey);
        // This test assumes that the shard key is on a single field.
        assert.eq(fields.length, 1);
        return fields[0];
    };

    $config.data.getExpectedErrors = function getExpectedErrors() {
        const expected = [];

        if (this.expectPartialMultiWrites) {
            expected.push(ErrorCodes.QueryPlanKilled);
        }

        return expected;
    };

    $config.data.getTargetedDocuments = function getTargetedDocuments(collName, query) {
        const shardKeyField = this.getShardKeyField(collName);
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
        const batchSize = 1 + Random.randInt(this.maxWriteBatchSize);
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
        const shardKeyField = this.getShardKeyField(collName);
        const randomDoc = chooseRandomMapValue(documents);
        query[shardKeyField] = randomDoc[shardKeyField];
        // Upsert will use the fields in the query to create the document, and we assume that at the
        // beginning of the test the shard key matches _id.
        if (shardKeyField !== "_id") {
            query["_id"] = query[shardKeyField];
        }
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
        const shardKeyField = this.getShardKeyField(collName);
        const randomDoc = chooseRandomMapValue(this.expectedDocs);
        query[shardKeyField] = randomDoc[shardKeyField];
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
        const shardKeyField = this.getShardKeyField(collName);
        const documents = findFirstBatch(db, collName, {tid: this.tid}, 1000);
        const map = new Map();
        for (const doc of documents) {
            map.set(doc[shardKeyField], doc);
        }
        return map;
    };

    $config.data.verifyDocumentCounters = function verifyDocumentCounters(onDiskBefore,
                                                                          onDiskAfter) {
        for (const key of this.expectedDocs.keys()) {
            // Verify the counter separately from the rest of the document to account for differing
            // semantics around retries.
            const expectedDoc = this.expectedDocs.get(key);
            const actualDoc = onDiskAfter.get(key);
            const priorDoc = onDiskBefore.get(key);
            if (actualDoc === undefined) {
                // An expected document is only allowed to be missing if it didn't exist prior to
                // executing the update batch. It's possible that we expected to upsert a document
                // later in the batch, but it was never executed because a multi update earlier in
                // the batch was killed.
                assert(priorDoc === undefined);
                assert(this.expectPartialMultiWrites);
                continue;
            }
            const {counter: expectedCounter, ...expectedDocNoCounter} = expectedDoc;
            const {counter: actualCounter, ...actualDocNoCounter} = actualDoc;
            const priorCounter = priorDoc ? priorDoc.counter : 0;
            assert.docEq(expectedDocNoCounter, actualDocNoCounter);
            assert(this.counterWithinRange(expectedCounter, actualCounter, priorCounter),
                   `Expected counter ${tojson(expectedDoc)} and on disk counter ${
                       tojson(actualDoc)} differ by more than allowed`);
        }
    };

    $config.data.counterWithinRange = function counterWithinRange(expected, actual, prior) {
        // It's always correct for the counter to be the expected value.
        if (actual === expected) {
            return true;
        }
        // For sharded collections, we expect to retry multi updates in some cases, which can lead
        // to updating the counter additional times.
        if (actual > expected) {
            return this.expectExtraMultiWrites;
        }
        // For unsharded collections or when failovers are enabled, we expect that sometimes a multi
        // update operation will be killed and fail to update some of the documents that match its
        // filter.
        if (actual < expected && actual >= prior) {
            return this.expectPartialMultiWrites;
        }
        // If we reach this far, it means the counter is lower than its value prior to executing the
        // updates. This should not be possible.
        return false;
    };

    $config.data.verifyUpdateResult = function verifyUpdateResult(expected, actual) {
        if (!this.expectPartialMultiWrites && !this.expectExtraMultiWrites) {
            // We don't expect partial nor extra updates, so the result must match exactly the
            // expected.
            assert.eq(actual.n, expected.n);
            assert.eq(actual.nModified, expected.nModified);
            return;
        }

        // When we expect extra multi updates, we can at least assert that at most two times the
        // number of actual documents were modified. This happens when a multi-update router with
        // ShardVersion::IGNORED completely executes on one shard before resharding commits, and on
        // the other after it commits.
        const multiplier = this.expectExtraMultiWrites ? 2 : 1;
        assert.lte(actual.n, multiplier * expected.n);
        assert.lte(actual.nModified, multiplier * expected.nModified);
    };

    $config.data.verifyDeleteResult = function verifyDeleteResult(expected, actual) {
        // In either case, if we can't expect a deleteMany to delete each matched document exactly
        // once, then we expect that the number must be less. If partial operations are possible,
        // then we could have been killed before deleting each document. If retried operations are
        // possible, we could have deleted some documents on the first pass, so they no longer exist
        // in the second pass.
        if (this.expectPartialMultiWrites || this.expectExtraMultiWrites) {
            assert.lte(actual, expected);
            return;
        }
        assert.eq(actual, expected);
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        findFirstBatch(db, collName, {}, 1000).forEach(doc => {
            const q = {_id: doc._id};
            let mods = {};
            if (!("tid" in doc)) {
                mods = {...mods, tid: Random.randInt($config.threadCount)};
            }
            if (!("counter" in doc)) {
                mods = {...mods, counter: 0};
            }
            db[collName].update(q, {$set: mods});
        });

        // Increase yielding so that there is more interleaving between operations.
        cluster.executeOnMongodNodes((db) => {
            const res = assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 50}));
            this.internalQueryExecYieldIterationsDefault = res.was;
        });
    };

    $config.teardown = function(db, collName, cluster) {
        cluster.executeOnMongodNodes((db) => {
            const res = assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: this.internalQueryExecYieldIterationsDefault
            }));
        });
    };

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
        this.initialDocs = this.readOwnedDocuments(db, collName);
        this.expectedDocs = new Map(this.initialDocs);
        jsTestLog(`Thread with tid ${this.tid} owns ${this.initialDocs.size} documents`);
    };

    $config.states.performUpdates = function multiUpdate(db, collName, connCache) {
        runWithManualRetriesIfInNonTransactionalStepdownSuite(() => {
            const onDiskBefore = this.readOwnedDocuments(db, collName);
            this.expectedDocs = new Map(onDiskBefore);
            const updates = this.createRandomUpdateBatch(collName);
            jsTestLog("Executing updates: " + tojson(updates));
            const result = db.runCommand({update: collName, updates});
            jsTestLog("Result: " + tojson(result));
            assert.commandWorkedOrFailedWithCode(result, this.getExpectedErrors());
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
            const onDiskAfter = this.readOwnedDocuments(db, collName);
            this.verifyDocumentCounters(onDiskBefore, onDiskAfter);
            this.verifyUpdateResult({n: totalUpdates, nModified: totalUpdates - totalUpserts},
                                    result);
        });
    };

    $config.states.performDeletes = function multiDelete(db, collName, connCache) {
        runWithManualRetriesIfInNonTransactionalStepdownSuite(() => {
            const onDiskBefore = this.readOwnedDocuments(db, collName);
            this.expectedDocs = new Map(onDiskBefore);
            const deletes = this.createRandomDeleteBatch(collName);
            jsTestLog("Executing deletes: " + tojson(deletes));
            const result = db.runCommand({delete: collName, deletes});
            jsTestLog("Result: " + tojson(result));
            assert.commandWorkedOrFailedWithCode(result, this.getExpectedErrors());
            let uniqueDeletes = new Set();
            for (const deleteOp of deletes) {
                const deletedKeys = this.getTargetedDocuments(collName, deleteOp.q);
                for (const key of deletedKeys) {
                    uniqueDeletes.add(key);
                    this.expectedDocs.delete(key);
                }
            }
            const onDiskAfter = this.readOwnedDocuments(db, collName);
            this.verifyDocumentCounters(onDiskBefore, onDiskAfter);
            this.verifyDeleteResult(uniqueDeletes.size, result.n);

            if (onDiskAfter.size > 0) {
                return;
            }
            jsTestLog(`Thread ${this.tid} has deleted all of its documents and will now reset to its initial state`);
            const bulk = db[collName].initializeUnorderedBulkOp();
            for (const [_, doc] of this.initialDocs) {
                bulk.insert(doc);
            }
            assert.commandWorked(bulk.execute());
        });
    };

    return $config;
}
