'use strict';

/**
 * Randomly performs a series of CRUD and movePrimary operations on unsharded collections, checking
 * for data consistency as a consequence of these operations.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_70
 *  ]
 */

load('jstests/libs/feature_flag_util.js');

const $config = (function() {
    const kCollNamePrefix = 'unsharded_coll_';
    const kInitialCollSize = 100;
    const kBatchSizeForDocsLookup = kInitialCollSize * 2;

    /**
     * Utility function that asserts that the specified command is executed successfully, i.e. that
     * no errors occur, or that any error is in `ignorableErrorCodes`. However, if the error is in
     * `retryableErrorCodes`, then the command is retried.
     */
    const assertCommandWorked = function(cmd, retryableErrorCodes, ignorableErrorCodes = []) {
        if (!Array.isArray(retryableErrorCodes)) {
            retryableErrorCodes = [retryableErrorCodes];
        }
        if (!Array.isArray(ignorableErrorCodes)) {
            ignorableErrorCodes = [ignorableErrorCodes];
        }

        let res = undefined;
        assertAlways.soon(() => {
            try {
                res = cmd();
                return true;
            } catch (err) {
                if (err instanceof BulkWriteError && err.hasWriteErrors()) {
                    for (let writeErr of err.getWriteErrors()) {
                        if (retryableErrorCodes.includes(writeErr.code)) {
                            return false;
                        } else if (ignorableErrorCodes.includes(writeErr.code)) {
                            continue;
                        } else {
                            throw err;
                        }
                    }
                    return true;
                } else if (retryableErrorCodes.includes(err.code)) {
                    return false;
                } else if (ignorableErrorCodes.includes(err.code)) {
                    return true;
                }
                throw err;
            }
        });
        return res;
    };

    const data = {
        // In-memory copy of the collection data. Every CRUD operation on the persisted collection
        // is reflected on this object. The collection consistency check is performed by comparing
        // its data with those managed by this copy.
        collMirror: {},

        // ID of the last document inserted into the collection. It's used as a generator of unique
        // IDs for new documents to insert.
        lastId: undefined,

        getRandomDoc: function() {
            const keys = Object.keys(this.collMirror);
            return this.collMirror[keys[Random.randInt(keys.length)]];
        }
    };

    const states = {
        init: function(db, collName, connCache) {
            // Insert an initial amount of documents into the collection, with a progressive _id and
            // the update counter set to zero.

            this.collName = `${kCollNamePrefix}${this.tid}`;
            let coll = db[this.collName];
            jsTestLog(`Initializing data: coll=${coll}`);

            for (let i = 0; i < kInitialCollSize; ++i) {
                this.collMirror[i] = {_id: i, updateCount: 0};
            }
            this.lastId = kInitialCollSize - 1;

            // Session with retryable writes is required to recover from a primary node step-down
            // event during bulk insert processing.
            this.session = db.getMongo().startSession({retryWrites: true});
            let sessionColl = this.session.getDatabase(db.getName()).getCollection(this.collName);

            assertCommandWorked(
                () => {
                    let bulkOp = sessionColl.initializeUnorderedBulkOp();
                    for (let i = 0; i < kInitialCollSize; ++i) {
                        bulkOp.insert(
                            {_id: i, updateCount: 0},
                        );
                    }
                    bulkOp.execute();
                },
                ErrorCodes.MovePrimaryInProgress,
                // TODO (SERVER-32113): Retryable writes may cause double inserts if performed on a
                // shard involved as the originator of a movePrimary operation.
                ErrorCodes.DuplicateKey);
        },
        insert: function(db, collName, connCache) {
            // Insert a document into the collection, with an _id greater than all those already
            // present (last + 1) and the update counter set to zero.

            let coll = db[this.collName];

            const newId = this.lastId += 1;
            jsTestLog(`Inserting document: coll=${coll} _id=${newId}`);

            this.collMirror[newId] = {_id: newId, updateCount: 0};

            assertCommandWorked(() => {
                coll.insertOne({_id: newId, updateCount: 0});
            }, ErrorCodes.MovePrimaryInProgress);
        },
        update: function(db, collName, connCache) {
            // Increment the update counter of a random document of the collection.

            let coll = db[this.collName];

            const randomId = this.getRandomDoc()._id;
            jsTestLog(`Updating document: coll=${coll} _id=${randomId}`);

            const newUpdateCount = this.collMirror[randomId].updateCount += 1;

            assertCommandWorked(() => {
                coll.updateOne({_id: randomId}, {$set: {updateCount: newUpdateCount}});
            }, ErrorCodes.MovePrimaryInProgress);
        },
        delete: function(db, collName, connCache) {
            // Remove a random document from the collection.

            let coll = db[this.collName];

            const randomId = this.getRandomDoc()._id;
            jsTestLog(`Deleting document: coll=${coll} _id=${randomId}`);

            delete this.collMirror[randomId];

            assertCommandWorked(() => {
                coll.deleteOne({_id: randomId});
            }, ErrorCodes.MovePrimaryInProgress);
        },
        movePrimary: function(db, collName, connCache) {
            // Move the primary shard of the database to a random shard (which could coincide with
            // the starting one).

            const shards = Object.keys(connCache.shards);
            const toShard = shards[Random.randInt(shards.length)];
            jsTestLog(`Running movePrimary: db=${db} to=${toShard}`);

            assertAlways.commandWorkedOrFailedWithCode(
                db.adminCommand({movePrimary: db.getName(), to: toShard}), [
                    // Caused by a concurrent movePrimary operation on the same database but a
                    // different destination shard.
                    ErrorCodes.ConflictingOperationInProgress,
                    // Due to a stepdown of the donor during the cloning phase, the movePrimary
                    // operation failed. It is not automatically recovered, but any orphaned data on
                    // the recipient has been deleted.
                    7120202,
                    // Same as the above, but due to a stepdown of the recipient.
                    ErrorCodes.MovePrimaryAborted
                ]);
        },
        checkDatabaseMetadataConsistency: function(db, collName, connCache) {
            if (this.skipMetadataChecks) {
                return;
            }
            jsTestLog('Executing checkMetadataConsistency state for database: ' + db.getName());
            const inconsistencies = db.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        checkCollectionMetadataConsistency: function(db, collName, connCache) {
            if (this.skipMetadataChecks) {
                return;
            }
            let coll = db[this.collName];
            jsTestLog(`Executing checkMetadataConsistency state for collection: ${coll}`);
            const inconsistencies = coll.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        verifyDocuments: function(db, collName, connCache) {
            // Verify the correctness of the collection data by checking that each document matches
            // its copy in memory.

            const coll = db[this.collName];
            jsTestLog(`Verifying data: coll=${coll}`);

            let docs = assertCommandWorked(
                () => {
                    return coll.find().batchSize(kBatchSizeForDocsLookup).toArray();
                },
                // Caused by a concurrent movePrimary operation.
                ErrorCodes.QueryPlanKilled);

            assertAlways.eq(Object.keys(this.collMirror).length,
                            docs.length,
                            `expectedData=${JSON.stringify(this.collMirror)}} actualData=${
                                JSON.stringify(docs)}`);

            docs.forEach(doc => {
                assertAlways.eq(this.collMirror[doc._id],
                                doc,
                                `expectedData=${JSON.stringify(this.collMirror)}} actualData=${
                                    JSON.stringify(docs)}`);
            });
        }
    };

    let setup = function(db, collName, cluster) {
        this.skipMetadataChecks =
            // TODO SERVER-70396: remove this flag
            !FeatureFlagUtil.isEnabled(db.getMongo(), 'CheckMetadataConsistency');
    };

    const standardTransition = {
        insert: 0.20,
        update: 0.20,
        delete: 0.20,
        movePrimary: 0.12,
        checkDatabaseMetadataConsistency: 0.04,
        checkCollectionMetadataConsistency: 0.04,
        verifyDocuments: 0.20,
    };

    const transitions = {
        init: standardTransition,
        insert: standardTransition,
        update: standardTransition,
        delete: standardTransition,
        movePrimary: standardTransition,
        checkDatabaseMetadataConsistency: standardTransition,
        checkCollectionMetadataConsistency: standardTransition,
        verifyDocuments: standardTransition
    };

    return {
        threadCount: 8,
        iterations: 32,
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        passConnectionCache: true
    };
})();
