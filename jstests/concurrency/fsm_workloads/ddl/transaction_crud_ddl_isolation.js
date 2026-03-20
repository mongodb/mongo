/**
 * Tests transaction isolation during concurrent DDL operations.
 *
 * This FSM workload runs CRUD transaction states (local/majority/snapshot read concerns)
 * concurrently with DDL states (blink rename, drop/recreate, and resharding). Collections are
 * created across multiple databases and may be untracked, unsplittable, or sharded.
 *
 * Anchor collection behavior:
 *  - With 50% probability, a transaction first reads `anchor_collection` on a random database to
 *    anchor/open its snapshot on a shard.
 *  - When anchored, the transaction sleeps briefly to widen the interleaving window for
 *    concurrent DDL while the transaction remains open.
 *
 * Validated invariants (when the transactional read returns documents):
 *  - The meta document exists and all documents have the same `collection_epoch` as meta.
 *  - Immutable seed documents are complete (100 docs), so partial snapshots are rejected.
 *  - For snapshot reads, linked seed pairs (i <-> i+50) have equal counters.
 *  - Thread-owned documents exactly match the tracked inserted-id set for that epoch.
 *
 * Document structure per collection:
 *  1. Meta document (1x):
 *     {_id: "meta", x: "meta", collection_epoch: <UUID>}
 *  2. Seed documents (100x):
 *     {_id: 0-99, x: 0-99, counter: 0, createdBy: "initial_docs", collection_epoch: <UUID>}
 *  3. Thread-owned documents (variable):
 *     {_id: <ObjectId>, x: <ObjectId>, createdBy: <tid>, collection_epoch: <UUID>}
 *
 * Note: All documents keep matching `_id` and `x` so both `{_id: "hashed"}` and
 * `{x: "hashed"}` shard key patterns are supported.
 *
 * @tags: [
 *   uses_transactions,
 * ]
 */

import {withTxnAndAutoRetry} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";
import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";

export const $config = (function () {
    const kMetaDocId = "meta";
    const kAnchorCollectionName = "anchor_collection";
    const kMaxMutableDocsPerTxn = 3;
    const readConcernLevels = ["local", "majority", "snapshot"];
    const shardKeyOptions = [{_id: "hashed"}, {x: "hashed"}];

    // Shared state configuration
    var data = {
        dbNamePrefix: jsTestName() + "_db_",
        collNamePrefix: "test_coll_",
        numDatabases: 2,
        numCollectionsPerDatabase: 2,
        numSeedDocuments: 100,
        maxSleepMs: 100,
    };
    assert.eq(0, data.numSeedDocuments % 2, "numSeedDocuments must be even");

    function getRandomDb(db) {
        const targetDbIndex = Random.randInt(data.numDatabases);
        const targetDbName = data.dbNamePrefix + targetDbIndex;
        return db.getSiblingDB(targetDbName);
    }

    function getRandomCollectionName() {
        const targetIndex = Random.randInt(data.numCollectionsPerDatabase);
        return data.collNamePrefix + targetIndex;
    }

    function getRandomTargetCollection(db) {
        const targetDb = getRandomDb(db);
        const collName = getRandomCollectionName();
        return targetDb.getCollection(collName);
    }

    function getRandomReadConcernLevel() {
        return readConcernLevels[Random.randInt(readConcernLevels.length)];
    }

    function randomSleep() {
        sleep(Random.randInt(data.maxSleepMs));
    }

    // With 50% probability, execute a read on the anchor collection on a random database to open
    // the transaction snapshot on a shard. Return true if the read was executed.
    function maybeAnchorTransactionOnRandomDb(session, db) {
        if (Random.rand() >= 0.5) {
            return false;
        }

        const anchorDb = getRandomDb(db);
        const anchorColl = session.getDatabase(anchorDb.getName()).getCollection(kAnchorCollectionName);
        anchorColl.findOne();
        return true;
    }

    // With 50% probability, flush the router config to force a metadata refresh on mongos.
    function maybeRefreshRoutingMetadata(targetColl) {
        if (Random.rand() <= 0.5) {
            return;
        }

        try {
            assert.commandWorked(
                targetColl.getDB().getMongo().adminCommand({flushRouterConfig: targetColl.getFullName()}),
            );
        } catch (e) {
            // May fail due to spurious error or interruptions. Ignore.
            print(`maybeRefreshRoutingMetadata: failed to flush router config for ${targetColl.getFullName()}: ${e}`);
        }
    }

    function getSessionCollection(session, targetColl) {
        return session.getDatabase(targetColl.getDB().getName()).getCollection(targetColl.getName());
    }

    function getEpochKey(collectionEpoch) {
        return collectionEpoch.toString();
    }

    function addTrackedIdsForEpoch(insertedIdsByEpoch, collectionEpoch, ids) {
        const epochKey = getEpochKey(collectionEpoch);
        if (!insertedIdsByEpoch[epochKey]) {
            insertedIdsByEpoch[epochKey] = new Set();
        }
        for (const id of ids) {
            insertedIdsByEpoch[epochKey].add(id);
        }
    }

    function removeTrackedIdsForEpoch(insertedIdsByEpoch, collectionEpoch, ids) {
        const epochKey = getEpochKey(collectionEpoch);
        const trackedIds = insertedIdsByEpoch[epochKey];
        if (!trackedIds) {
            return;
        }
        for (const id of ids) {
            trackedIds.delete(id);
        }
    }

    function pickRandomDistinctElements(items, count) {
        const pickedItems = [];
        const availableIndices = items.map((_, index) => index);
        for (let i = 0; i < count; i++) {
            const pickIndex = Random.randInt(availableIndices.length);
            const sourceIndex = availableIndices[pickIndex];
            pickedItems.push(items[sourceIndex]);
            availableIndices.splice(pickIndex, 1);
        }
        return pickedItems;
    }

    /**
     * Helper function to create and populate a collection.
     * Handles collection creation, optional sharding, and bulk insert of test data.
     *
     * @param {object} self - The 'this' context from the calling function (to access isShardedCluster)
     * @param {DB} db - Database object
     * @param {string} collectionName - Name of the collection to create
     */
    function createAndPopulateCollection(self, db, collectionName) {
        const dbName = db.getName();
        const ns = dbName + "." + collectionName;

        // Randomly decide collection type in sharded cluster:
        //    a) Untracked - just create the collection
        //    b) Tracked unsplittable - createUnsplittableCollection on a random data shard
        //    c) Sharded - shardCollection
        if (self.isShardedCluster) {
            const configDB = db.getSiblingDB("config");
            const shards = configDB.shards.find().toArray();
            const rand = Random.rand();
            let collType;
            if (rand < 0.33) {
                // a) Untracked collection
                assert.commandWorked(db.createCollection(collectionName));
                collType = "untracked";
            } else if (rand < 0.66) {
                // b) Tracked unsplittable on a randomly chosen data shard
                const targetShard = shards[Random.randInt(shards.length)]._id;
                assert.commandWorked(
                    db.runCommand({createUnsplittableCollection: collectionName, dataShard: targetShard}),
                );
                collType = "unsplittable on " + targetShard;
            } else {
                // c) Sharded collection with a randomly chosen shard key
                const shardKey = shardKeyOptions[Random.randInt(shardKeyOptions.length)];
                assert.commandWorked(db.createCollection(collectionName));
                assert.commandWorked(db.adminCommand({shardCollection: ns, key: shardKey}));
                collType = "sharded with key " + tojson(shardKey);
            }
            print(`Setup: created ${ns} (${collType})`);
        } else {
            assert.commandWorked(db.createCollection(collectionName));
            print(`Setup: created ${ns} (unsharded cluster)`);
        }

        // 3. Insert the initial documents
        // Each document has an 'x' field that matches its '_id' to support both shard key patterns
        var bulk = db[collectionName].initializeUnorderedBulkOp();
        const collectionEpoch = UUID();
        bulk.insert({_id: kMetaDocId, x: kMetaDocId, collection_epoch: collectionEpoch});
        for (let i = 0; i < data.numSeedDocuments; i++) {
            bulk.insert({_id: i, x: i, counter: 0, createdBy: "initial_docs", collection_epoch: collectionEpoch});
        }
        var result = bulk.execute();
        assert.eq(
            result.nInserted,
            data.numSeedDocuments + 1,
            `Failed to insert all documents: expected ${data.numSeedDocuments + 1}, got ${result.nInserted}`,
        );
        print(`createAndPopulateCollection: created ${ns} with collection_epoch=${collectionEpoch}`);
    }

    // Create N databases, each with M collections, randomly sharded or unsharded.
    var setup = function (db, collName, cluster) {
        this.isShardedCluster = cluster.isSharded();

        // Create collections across multiple databases
        for (let dbIndex = 0; dbIndex < data.numDatabases; dbIndex++) {
            const currentDbName = data.dbNamePrefix + dbIndex;
            const currentDb = db.getSiblingDB(currentDbName);

            for (let collIndex = 0; collIndex < data.numCollectionsPerDatabase; collIndex++) {
                const currentCollName = data.collNamePrefix + collIndex;
                createAndPopulateCollection(this, currentDb, currentCollName);
            }

            // Ensure the anchor collection exists for read transactions.
            // Threads may touch this collection first to anchor a transaction snapshot on a shard.
            assert.commandWorked(currentDb.createCollection(kAnchorCollectionName));
            assert.commandWorked(currentDb[kAnchorCollectionName].insert({_id: 1, x: 1}));
        }
    };

    // Create session for this thread to reuse across states.
    var init = function (db, collName) {
        this.session = db.getMongo().startSession();

        // Track _ids of documents inserted by this thread, keyed by collection epoch.
        // Each entry maps collection_epoch -> Set(_id values).
        this.insertedIds = {};
    };

    // Validate the documents read from the collection.
    function validateDocuments(self, docs, targetColl, readConcernLevel) {
        const metaDoc = docs.find((d) => d._id === kMetaDocId);
        assert(
            metaDoc,
            `Meta document is expected to exist. Collection: ${targetColl.getFullName()}, ` +
                `readConcern: ${readConcernLevel}, docs: ${tojson(docs)}`,
        );
        const collectionEpoch = metaDoc.collection_epoch;
        const docsWithUnexpectedEpoch = docs.filter(
            (d) => !d.hasOwnProperty("collection_epoch") || !bsonBinaryEqual(d.collection_epoch, collectionEpoch),
        );
        assert.eq(
            0,
            docsWithUnexpectedEpoch.length,
            `All docs should have collection_epoch=${collectionEpoch}. Collection: ${targetColl.getFullName()}, ` +
                `readConcern: ${readConcernLevel}, badDocs: ${tojson(docsWithUnexpectedEpoch)}`,
        );

        // Expect to see all the seed documents.
        const seedDocs = docs.filter((d) => d.createdBy === "initial_docs");
        assert(
            seedDocs.length === data.numSeedDocuments,
            `Inconsistent Snapshot! Expected ${data.numSeedDocuments} seed docs but got ${seedDocs.length}. ` +
                `Collection: ${targetColl.getFullName()}, collection_epoch: ${collectionEpoch}, ` +
                `readConcern: ${readConcernLevel}, docs: ${tojson(docs)}`,
        );

        // Validate seed linked-pair invariant. Can only be checked when readConcern is snapshot.
        if (readConcernLevel === "snapshot") {
            const linkedPairOffset = data.numSeedDocuments / 2;
            const seedDocsById = seedDocs.slice().sort((a, b) => a._id - b._id);
            for (let baseId = 0; baseId < linkedPairOffset; baseId++) {
                const baseDoc = seedDocsById[baseId];
                const pairedDoc = seedDocsById[baseId + linkedPairOffset];
                assert.eq(baseId, baseDoc._id, `Unexpected seed doc at index ${baseId}: ${tojson(baseDoc)}`);
                assert.eq(
                    baseId + linkedPairOffset,
                    pairedDoc._id,
                    `Unexpected paired seed doc at index ${baseId + linkedPairOffset}: ${tojson(pairedDoc)}`,
                );
                assert.eq(
                    baseDoc.counter,
                    pairedDoc.counter,
                    `Linked pair counter mismatch! Collection: ${targetColl.getFullName()}, ` +
                        `collection_epoch: ${collectionEpoch}, readConcern: ${readConcernLevel}, ` +
                        `baseDoc: ${tojson(baseDoc)}, pairedDoc: ${tojson(pairedDoc)}`,
                );
            }
        }

        // Expect to see all thread-owned documents created by this thread, with no extras.
        const epochKey = getEpochKey(collectionEpoch);
        const docsInsertedByThisThread = self.insertedIds[epochKey] || new Set();
        const expectedThreadDocIds = new Set(Array.from(docsInsertedByThisThread).map((id) => id.toString()));

        const readThreadDocs = docs.filter((d) => d.createdBy === self.tid);
        const actualThreadDocIds = new Set(readThreadDocs.map((d) => d._id.toString()));

        assert.setEq(
            expectedThreadDocIds,
            actualThreadDocIds,
            `Thread-owned document membership mismatch! Collection: ${targetColl.getFullName()}, ` +
                `collection_epoch: ${collectionEpoch}, readConcern: ${readConcernLevel}, ` +
                `readThreadDocs: ${tojson(readThreadDocs)}, expectedIds: [${Array.from(expectedThreadDocIds).join(", ")}], ` +
                `actualIds: [${Array.from(actualThreadDocIds).join(", ")}]`,
        );
    }

    // Transactional Read. Checks invariants of a random collection.
    var scanDataInTransaction = function (db, collName) {
        // This function uses its own transaction session, so we don't want the FSM framework to try to wrap it in a different one.
        fsm.forceRunningOutsideTransaction(this);

        // Randomly pick a database and collection to read.
        const targetColl = getRandomTargetCollection(db);

        // Randomly pick a read concern level.
        const readConcernLevel = getRandomReadConcernLevel();

        // Execute the transaction.
        const docs = withTxnAndAutoRetry(
            this.session,
            () => {
                // Optionally read the anchor collection on a random database first.
                // This anchors the transaction snapshot on a shard, creating a time
                // gap between when this shard picks its read timestamp vs other shards (which
                // will pick theirs later, after the sleep). This is useful for testing
                // non-snapshot read concerns.
                if (maybeAnchorTransactionOnRandomDb(this.session, db)) {
                    randomSleep();
                }

                maybeRefreshRoutingMetadata(targetColl);

                // Now read the target collection - fetch all documents in one query
                const coll = getSessionCollection(this.session, targetColl);
                return coll.find({}).toArray();
            },
            {txnOptions: {readConcern: {level: readConcernLevel}}},
        );

        print(
            `scanDataInTransaction: read ${docs.length} docs from ${targetColl.getFullName()}, ` +
                `readConcern=${readConcernLevel}`,
        );

        if (docs.length === 0) {
            return;
        }

        validateDocuments(this, docs, targetColl, readConcernLevel);
    };

    // Transactional Update Pair.
    // Atomically increments counter on both documents in a linked pair (id <-> id+50)
    // This ensures the invariant doc[x].counter === doc[x+50].counter is maintained
    var transactionalUpdatePair = function (db, collName) {
        // This function uses its own transaction session, so we don't want the FSM framework to try to wrap it in a different one.
        fsm.forceRunningOutsideTransaction(this);

        // Randomly pick a read concern level.
        const readConcernLevel = getRandomReadConcernLevel();

        // Randomly pick a collection to update
        const targetColl = getRandomTargetCollection(db);

        withTxnAndAutoRetry(
            this.session,
            () => {
                const coll = getSessionCollection(this.session, targetColl);

                // Optionally anchor the transaction on a random database first
                if (maybeAnchorTransactionOnRandomDb(this.session, db)) {
                    randomSleep();
                }

                maybeRefreshRoutingMetadata(targetColl);

                // Pick a random document from the first half.
                const linkedPairOffset = data.numSeedDocuments / 2;
                const baseId = Random.randInt(linkedPairOffset);
                const pairedId = baseId + linkedPairOffset;

                // Update the first document in the pair
                const result1 = coll.updateOne({_id: baseId}, {$inc: {counter: 1}});

                randomSleep();

                // Update the second document in the pair
                const result2 = coll.updateOne({_id: pairedId}, {$inc: {counter: 1}});

                const matchedCount1 = result1.matchedCount;
                const matchedCount2 = result2.matchedCount;

                // Both updates should either match (both docs present) or miss (collection renamed away).
                assert.eq(
                    matchedCount1,
                    matchedCount2,
                    `Inconsistent snapshot in update! _id=${baseId} matched=${matchedCount1}, ` +
                        `_id=${pairedId} matched=${matchedCount2}. Namespace: ${coll.getFullName()}`,
                );
                assert(
                    matchedCount1 === 0 || matchedCount1 === 1,
                    `Unexpected matchedCount while updating linked pair (${baseId}, ${pairedId}) in ` +
                        `${targetColl.getFullName()}: matchedCount=${matchedCount1}`,
                );

                if (matchedCount1 > 0) {
                    print(
                        `transactionalUpdatePair: updated pair (${baseId},${pairedId}) in ${targetColl.getFullName()}, ` +
                            `readConcern=${readConcernLevel}`,
                    );
                } else {
                    print(
                        `transactionalUpdatePair: skipped update of pair (${baseId},${pairedId}) in ${targetColl.getFullName()}, ` +
                            `readConcern=${readConcernLevel}`,
                    );
                }
            },
            {txnOptions: {readConcern: {level: readConcernLevel}}},
        );
    };

    // Transactional Insert.
    // Inserts 1-3 documents in a multi-document transaction and tracks them with the collection
    // epoch for later deletion by specific _id.
    var transactionalInsert = function (db, collName) {
        // This function uses its own transaction session, so we don't want the FSM framework to try to wrap it in a different one.
        fsm.forceRunningOutsideTransaction(this);

        const targetColl = getRandomTargetCollection(db);
        const numDocsToInsert = Random.randInt(kMaxMutableDocsPerTxn) + 1; // 1 to 3 documents

        // Randomly pick a read concern level.
        const readConcernLevel = getRandomReadConcernLevel();

        // Generate documents with explicit _ids so we can track them for later deletion.
        // Each doc has createdBy field to identify which thread inserted it.
        // The 'x' field matches '_id' to support both shard key patterns.
        const docsToInsert = [];
        for (let i = 0; i < numDocsToInsert; i++) {
            const docId = new ObjectId();
            docsToInsert.push({
                _id: docId,
                x: docId,
                createdBy: this.tid,
            });
        }

        const insertOutcome = withTxnAndAutoRetry(
            this.session,
            () => {
                // Optionally anchor the transaction on a random database first
                if (maybeAnchorTransactionOnRandomDb(this.session, db)) {
                    randomSleep();
                }

                maybeRefreshRoutingMetadata(targetColl);

                const coll = getSessionCollection(this.session, targetColl);

                // Read the meta document to get the collection epoch
                const metaDoc = coll.findOne({_id: kMetaDocId});
                if (!metaDoc) {
                    // Coll does not exist. Skip state to avoid implicit recreation.
                    return null;
                }
                const collectionEpoch = metaDoc.collection_epoch;
                const docsToInsertWithEpoch = docsToInsert.map((doc) =>
                    Object.assign({}, doc, {collection_epoch: collectionEpoch}),
                );

                // Insert all documents in the transaction
                const insertResult = assert.commandWorked(coll.insertMany(docsToInsertWithEpoch));
                assert.eq(insertResult.insertedIds.length, numDocsToInsert);
                return {
                    collectionEpoch,
                    insertedIds: docsToInsert.map((doc) => doc._id),
                };
            },
            {txnOptions: {readConcern: {level: readConcernLevel}}},
        );

        // If the transaction succeeded and we inserted docs, track the _ids
        if (insertOutcome !== null) {
            addTrackedIdsForEpoch(this.insertedIds, insertOutcome.collectionEpoch, insertOutcome.insertedIds);
            print(
                `transactionalInsert: inserted ${insertOutcome.insertedIds.length} docs in ${targetColl.getFullName()}, ` +
                    `_ids=[${insertOutcome.insertedIds.map((id) => id.toString()).join(", ")}], collection_epoch=${insertOutcome.collectionEpoch}, ` +
                    `readConcern=${readConcernLevel}`,
            );
        } else {
            print(
                `transactionalInsert: skipped insertion of docs in ${targetColl.getFullName()}, ` +
                    `collection does not exist. readConcern=${readConcernLevel}`,
            );
        }
    };

    // Transactional Delete.
    // Deletes 1-3 documents by specific _id that this thread had previously inserted.
    var transactionalDelete = function (db, collName) {
        // This function uses its own transaction session, so we don't want the FSM framework to try to wrap it in a different one.
        fsm.forceRunningOutsideTransaction(this);

        const targetColl = getRandomTargetCollection(db);

        // Randomly pick a read concern level.
        const readConcernLevel = getRandomReadConcernLevel();

        const deleteOutcome = withTxnAndAutoRetry(
            this.session,
            () => {
                // Optionally anchor the transaction on a random database first
                if (maybeAnchorTransactionOnRandomDb(this.session, db)) {
                    randomSleep();
                }

                maybeRefreshRoutingMetadata(targetColl);

                const coll = getSessionCollection(this.session, targetColl);

                // Read the meta document to get the collection epoch
                const metaDoc = coll.findOne({_id: kMetaDocId});
                if (!metaDoc) {
                    // Coll does not exist. Skip state to avoid implicit recreation.
                    return null;
                }
                const collectionEpoch = metaDoc.collection_epoch;
                const epochKey = getEpochKey(collectionEpoch);

                // Check how many docs this thread has tracked for this collection epoch
                const trackedIdsSet = this.insertedIds[epochKey];
                if (!trackedIdsSet || trackedIdsSet.size === 0) {
                    // No tracked docs to delete for this epoch.
                    return null;
                }

                // Convert Set to Array for random selection
                const trackedIdsArray = Array.from(trackedIdsSet);

                // Delete between 1 and 3 documents, but no more than what we have tracked
                const numToDelete = Math.min(Random.randInt(kMaxMutableDocsPerTxn) + 1, trackedIdsArray.length);
                const idsToDelete = pickRandomDistinctElements(trackedIdsArray, numToDelete);

                // Delete documents by specific _id
                const deletedIds = [];
                for (const idToDelete of idsToDelete) {
                    const result = coll.deleteOne({_id: idToDelete});
                    if (result.deletedCount > 0) {
                        deletedIds.push(idToDelete);
                    }
                }

                return {collectionEpoch, deletedIds};
            },
            {txnOptions: {readConcern: {level: readConcernLevel}}},
        );

        // After successful commit, remove the deleted _ids from our tracking
        if (deleteOutcome !== null && deleteOutcome.deletedIds.length > 0) {
            removeTrackedIdsForEpoch(this.insertedIds, deleteOutcome.collectionEpoch, deleteOutcome.deletedIds);
            print(
                `transactionalDelete: deleted ${deleteOutcome.deletedIds.length} docs from ${targetColl.getFullName()}, ` +
                    `_ids=[${deleteOutcome.deletedIds.map((id) => id.toString()).join(", ")}], collection_epoch=${deleteOutcome.collectionEpoch}, ` +
                    `readConcern=${readConcernLevel}`,
            );
        } else {
            print(
                `transactionalDelete: skipped deletion of docs in ${targetColl.getFullName()}, ` +
                    `no docs to delete or collection does not exist. readConcern=${readConcernLevel}`,
            );
        }
    };

    // Rename a random collection to a temporary name and then back to the original name.
    var randomRename = function (db, collName) {
        // Rename cannot be run inside a multi-statement transaction.
        fsm.forceRunningOutsideTransaction(this);

        const targetDb = getRandomDb(db);
        const targetIndex = Random.randInt(data.numCollectionsPerDatabase);
        const sourceColl = data.collNamePrefix + targetIndex;
        const tempColl = data.collNamePrefix + targetIndex + "_temp_rename";
        const ns = targetDb.getName() + "." + sourceColl;

        try {
            // We do a "blink" rename: A -> B -> A
            // This ensures the collection mostly exists for readers, but creates the race window.

            // 1. Rename Source -> Temp
            // dropTarget: true ensures if temp exists from a previous crash, we overwrite it
            targetDb[sourceColl].renameCollection(tempColl, true);

            randomSleep();

            // 2. Rename Temp -> Source
            targetDb[tempColl].renameCollection(sourceColl, true);

            print(`randomRename: completed blink rename on ${ns}`);
        } catch (e) {
            // Ignore errors (e.g. source doesn't exist, namespace conflict from concurrent DDL).
            print(`randomRename: error on ${ns}: ${e.message}`);
        }
    };

    // Reshard a random collection with a randomly chosen shard key.
    // Only executes on sharded clusters.
    var randomReshardCollection = function (db, collName) {
        // Resharding cannot be run inside a multi-statement transaction.
        fsm.forceRunningOutsideTransaction(this);

        if (!this.isShardedCluster) {
            // Resharding only applies to sharded clusters.
            return;
        }

        const targetColl = getRandomTargetCollection(db);

        // Randomly choose between the two shard key patterns
        const newShardKey = shardKeyOptions[Random.randInt(shardKeyOptions.length)];

        try {
            print(
                `randomReshardCollection: starting reshard of ${targetColl.getFullName()} with key ${tojson(newShardKey)}`,
            );

            assert.commandWorked(
                db.adminCommand({
                    reshardCollection: targetColl.getFullName(),
                    key: newShardKey,
                }),
            );

            print(
                `randomReshardCollection: completed reshard of ${targetColl.getFullName()} with key ${tojson(newShardKey)}`,
            );
        } catch (e) {
            // Only ignore NamespaceNotFound or NamespaceNotSharded (e.g. collection doesn't exist
            // or is not sharded). Other errors are unexpected.
            const allowedCodes = [
                ErrorCodes.NamespaceNotFound,
                ErrorCodes.NamespaceNotSharded,
                ErrorCodes.ReshardCollectionInProgress,
                ErrorCodes.ConflictingOperationInProgress,
            ];
            if (!e.code || !allowedCodes.includes(e.code)) {
                throw e;
            }
            print(`randomReshardCollection: error on ${targetColl.getFullName()}: ${e.message}`);
        }
    };

    // Drop a random collection and recreate it.
    var randomDropAndRecreateCollection = function (db, collName) {
        // Drop and recreate cannot be run inside a multi-statement transaction.
        fsm.forceRunningOutsideTransaction(this);

        const targetColl = getRandomTargetCollection(db);
        const targetDb = targetColl.getDB();
        const targetCollName = targetColl.getName();
        const ns = targetColl.getFullName();
        const tempColl = `${targetCollName}_temp_recreate_${this.tid}`;

        try {
            targetColl.drop();
            randomSleep();

            // Recreate via temp collection + rename to ensure atomicity
            createAndPopulateCollection(this, targetDb, tempColl);
            targetDb[tempColl].renameCollection(targetCollName, true /* dropTarget */);

            print(`randomDropAndRecreateCollection: completed on ${ns}`);
        } catch (e) {
            // Clean up temp collection on error.
            targetDb[tempColl].drop();
            print(`randomDropAndRecreateCollection: error on ${ns}: ${e.message}`);
        }
    };

    var teardown = function (db, collName, cluster) {};

    const workingStateTransitions = {
        scanDataInTransaction: 0.25,
        transactionalUpdatePair: 0.2,
        transactionalInsert: 0.2,
        transactionalDelete: 0.2,
        randomRename: 0.05,
        randomDropAndRecreateCollection: 0.05,
        randomReshardCollection: 0.05,
    };

    const states = {
        init: init,
        scanDataInTransaction: scanDataInTransaction,
        transactionalUpdatePair: transactionalUpdatePair,
        transactionalInsert: transactionalInsert,
        transactionalDelete: transactionalDelete,
        randomRename: randomRename,
        randomDropAndRecreateCollection: randomDropAndRecreateCollection,
        randomReshardCollection: randomReshardCollection,
    };

    var transitions = {
        // init transitions to working states and never returns
        init: workingStateTransitions,
    };
    Object.keys(states).forEach((stateName) => {
        if (stateName !== "init") {
            transitions[stateName] = workingStateTransitions;
        }
    });

    return {
        threadCount: 5,
        iterations: 100,
        startState: "init",
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
