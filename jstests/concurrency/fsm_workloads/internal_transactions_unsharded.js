'use strict';

/**
 * Runs insert, update, delete and findAndModify commands in internal transactions using all the
 * available client session settings. This workload works on both standalone replica sets and
 * sharded clusters since by default the FSM runner shards every collection used by a workload that
 * runs against a sharded cluster using the shard key {_id: hashed}. However, the workload is only
 * run on standalone replica sets since there is already a sharded workload for this
 * (internal_transactions_sharded.js) that sets up its own range-sharded collection.
 *
 * @tags: [
 *  requires_fcv_60,
 *  requires_persistence,
 *  uses_transactions,
 *  assumes_unsharded_collection
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js');
load("jstests/libs/override_methods/retry_writes_at_least_once.js");

// This workload involves running commands outside a session.
TestData.disableImplicitSessions = true;

if ($config === undefined) {
    // There is no workload to extend. Define a noop base workload to make the 'extendWorkload' call
    // below still work.
    $config = {
        threadCount: 1,
        iterations: 1,
        startState: "init",
        data: {},
        states: {init: function(db, collName) {}},
        transitions: {init: {init: 1}},
        setup: function(db, collName) {},
        teardown: function(db, collName) {},
    };
}

var $config = extendWorkload($config, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    // The number of documents assigned to a thread when the workload starts.
    $config.data.partitionSize = 200;
    // The counter values for the documents assigned to a thread. The map is populated during
    // the init state and is updated after every write in the other states. Used to verify that
    // updates aren't double applied.
    $config.data.expectedCounters = {};

    // This workload sets the 'storeFindAndModifyImagesInSideCollection' parameter to a random bool
    // during setup() and restores the original value during teardown().
    $config.data.originalStoreFindAndModifyImagesInSideCollection = undefined;

    // Determine if this workload needs to use causally consistent sessions.
    $config.data.shouldUseCausalConsistency = (() => {
        if (TestData.runningWithCausalConsistency !== undefined) {
            // Use the causal consistency setting on TestData.
            return TestData.runningWithCausalConsistency;
        }
        if (TestData.runningWithShardStepdowns) {
            // Use causal consistency since it is running with stepdown/kill/terminate and "read
            // your own writes" against the primary is only guaranteed outside a casually consistent
            // session when the primary is stable.
            return true;
        }
        // Don't use causal consistency in other cases since it should not be necessary.
        return false;
    })();

    const executionContextTypes =
        {kNoClientSession: 1, kClientSession: 2, kClientRetryableWrite: 3, kClientTransaction: 4};
    const imageTypes = {kPreImage: 1, kPostImage: 2};

    /**
     * Returns a random boolean.
     */
    $config.data.generateRandomBool = function generateRandomBool() {
        return Math.random() > 0.5;
    };

    /**
     * Returns a random integer between min (inclusive) and max (inclusive).
     */
    $config.data.generateRandomInt = function generateRandomInt(min, max) {
        return Math.floor(Math.random() * (max - min + 1)) + min;
    };

    $config.data.generateRandomExecutionContext = function generateRandomExecutionContext() {
        if (this.shouldUseCausalConsistency) {
            // Exclude kNoClientSession since a (casually consistent) session is required.
            return this.generateRandomInt(2, 4);
        }
        return this.generateRandomInt(1, 4);
    };

    $config.data.generateRandomImageType = function generateRandomImageType() {
        return this.generateRandomInt(1, 2);
    };

    $config.data.getDB = function getDB(db, executionCtxType) {
        const dbName = db.getName();

        switch (executionCtxType) {
            case executionContextTypes.kNoClientSession:
                return db.getMongo().getDB(dbName);
            case executionContextTypes.kClientSession:
                return this.nonRetryableWriteSession.getDatabase(dbName);
            case executionContextTypes.kClientRetryableWrite:
                return this.retryableWriteSession.getDatabase(dbName);
            case executionContextTypes.kClientTransaction:
                return this.generateRandomBool() ? this.nonRetryableWriteSession.getDatabase(dbName)
                                                 : this.retryableWriteSession.getDatabase(dbName);
            default:
                throw Error("Unknown execution context");
        }
    };

    $config.data.getMaxClusterTime = function getMaxClusterTime(sessions) {
        let maxClusterTime = new Timestamp(1, 0);
        for (let session of sessions) {
            if (session.getClusterTime() === undefined) {
                continue;
            }
            const clusterTime = session.getClusterTime().clusterTime;
            if (clusterTime > maxClusterTime) {
                maxClusterTime = clusterTime;
            }
        }
        return maxClusterTime;
    };

    const insertOpFieldName = "insertOp";
    const updateOpFieldName = "updateOp";
    const findAndModifyOpFieldName = "findAndModifyOp";

    $config.data.getQueryForDocument = function getQueryForDocument(collection, doc) {
        return {_id: doc._id, tid: this.tid};
    };

    $config.data.getRandomDocument = function getRandomDocument(collection) {
        const doc = collection.findOne({tid: this.tid});
        assert.neq(doc, null);
        return doc;
    };

    $config.data.generateRandomDocument = function generateRandomDocument() {
        return {_id: UUID(), tid: this.tid, counter: 0};
    };

    $config.data.generateRandomInsert = function generateRandomInsert(collection) {
        const docToInsert = this.generateRandomDocument(collection);
        docToInsert[insertOpFieldName] = 0;
        return docToInsert;
    };

    $config.data.generateRandomUpdate = function generateRandomUpdate(collection) {
        const docToUpdate = this.getRandomDocument(collection);
        assert.neq(docToUpdate, null);

        const updatedDoc = Object.assign({}, docToUpdate);
        updatedDoc[updateOpFieldName] = docToUpdate.hasOwnProperty(updateOpFieldName)
            ? (docToUpdate[updateOpFieldName] + 1)
            : 0;
        updatedDoc.counter += 1;

        const update = {
            $set: {[updateOpFieldName]: updatedDoc[updateOpFieldName]},
            $inc: {counter: 1}
        };

        return {docToUpdate, updatedDoc, update};
    };

    $config.data.generateRandomDelete = function generateRandomDelete(collection) {
        const docToDelete = this.getRandomDocument(collection);
        assert.neq(docToDelete, null);
        return docToDelete;
    };

    $config.data.generateRandomFindAndModify = function generateRandomFindAndModify(collection) {
        const isUpsert = this.generateRandomBool();
        const imageType = this.generateRandomImageType();

        const docToUpdate =
            isUpsert ? this.generateRandomDocument(collection) : this.getRandomDocument(collection);

        const updatedDoc = Object.assign({}, docToUpdate);
        updatedDoc[findAndModifyOpFieldName] = docToUpdate.hasOwnProperty(findAndModifyOpFieldName)
            ? (docToUpdate[findAndModifyOpFieldName] + 1)
            : 0;
        updatedDoc.counter += 1;

        const update = {
            $set: {[findAndModifyOpFieldName]: updatedDoc[findAndModifyOpFieldName]},
            $inc: {counter: 1}
        };

        return {docToUpdate, updatedDoc, update, isUpsert, imageType};
    };

    /**
     * Runs the given the write command 'writeCmdObj' inside an internal transaction using the given
     * client 'executionCtxType'.
     */
    $config.data.runInternalTransaction = function runInternalTransaction(
        db, collection, executionCtxType, writeCmdObj, checkResponseFunc, checkDocsFunc) {
        if (executionCtxType == executionContextTypes.kClientRetryableWrite) {
            writeCmdObj.stmtId = NumberInt(1);
        }

        // Add an insert command to each transaction so that when this workload is running on a
        // sharded cluster there can be a mix of single-shard and cross-shard transactions.
        const docToInsert = this.generateRandomInsert(collection);
        const insertCmdObj = {insert: collection.getName(), documents: [docToInsert]};
        if (executionCtxType == executionContextTypes.kClientRetryableWrite) {
            insertCmdObj.stmtId = NumberInt(-1);
        }

        const testInternalTxnCmdObj = {
            testInternalTransactions: 1,
            commandInfos: [
                {dbName: db.getName(), command: writeCmdObj},
                {dbName: db.getName(), command: insertCmdObj}
            ],
        };
        print(`Running an internal transaction using a test command ${
            tojsononeline(testInternalTxnCmdObj)}: ${tojsononeline({executionCtxType})}`);

        let runFunc = () => {
            const res = assert.commandWorked(db.adminCommand(testInternalTxnCmdObj));
            print(`Response: ${tojsononeline(res)}`);
            res.responses.forEach(response => assert.commandWorked(response));
            if (executionCtxType == executionContextTypes.kClientRetryableWrite) {
                // If the command was retried, 'responses' would only contain the response for
                // 'writeCmdObj'.
                assert.lte(res.responses.length, 2);
            } else {
                assert.eq(res.responses.length, 2);
            }

            const writeCmdRes = res.responses[0];
            checkResponseFunc(writeCmdRes);
            if (res.responses.length == 2) {
                const insertCmdRes = res.responses[1];
                assert.eq(insertCmdRes.n, 1, insertCmdRes);
            }
        };

        if (executionCtxType == executionContextTypes.kClientTransaction) {
            withTxnAndAutoRetry(
                db.getSession(), runFunc, {retryOnKilledSession: this.retryOnKilledSession});
        } else {
            runFunc();
        }

        checkDocsFunc();
        assert.eq(collection.findOne({_id: docToInsert._id}), docToInsert);
        this.expectedCounters[docToInsert._id] = docToInsert.counter;
    };

    $config.setup = function setup(db, collName, cluster) {
        assert.commandWorked(db.createCollection(collName, {writeConcern: {w: "majority"}}));

        // Store the findAndModify images in the oplog half of the time.
        const enableFindAndModifyImageCollection = this.generateRandomBool();
        this.originalStoreFindAndModifyImagesInSideCollection =
            assert
                .commandWorked(db.adminCommand({
                    setParameter: 1,
                    storeFindAndModifyImagesInSideCollection: enableFindAndModifyImageCollection
                }))
                .was;
    };

    $config.teardown = function teardown(db, collName, cluster) {
        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            storeFindAndModifyImagesInSideCollection:
                this.originalStoreFindAndModifyImagesInSideCollection
        }));
    };

    /**
     * Starts a retryable-write session and non-retryable write session, inserts the documents for
     * this thread, and populates the 'expectedCounters' map.
     */
    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        this.nonRetryableWriteSession = db.getMongo().startSession(
            {causalConsistency: this.shouldUseCausalConsistency, retryWrites: false});
        this.retryableWriteSession = db.getMongo().startSession(
            {causalConsistency: this.shouldUseCausalConsistency, retryWrites: true});
        print(`Started a non-retryable write session ${
            tojsononeline(this.nonRetryableWriteSession.getSessionId())}`);
        print(`Started a retryable write session ${
            tojsononeline(this.retryableWriteSession.getSessionId())}`);

        const collection = db.getCollection(collName);
        let bulk = collection.initializeUnorderedBulkOp();
        for (let i = 0; i < this.partitionSize; ++i) {
            const doc = this.generateRandomDocument(collection);
            bulk.insert(doc);
            this.expectedCounters[doc._id] = doc.counter;
        }
        assert.commandWorked(bulk.execute());
    };

    $config.states.internalTransactionForInsert = function internalTransactionForInsert(db,
                                                                                        collName) {
        const executionCtxType = this.generateRandomExecutionContext();
        db = this.getDB(db, executionCtxType);
        const collection = db.getCollection(collName);

        const docToInsert = this.generateRandomInsert(collection);
        const insertCmdObj = {insert: collection.getName(), documents: [docToInsert]};
        const checkResponseFunc = (res) => {
            assert.eq(res.n, 1, res);
        };
        const checkDocsFunc = () => {
            assert.eq(collection.findOne({_id: docToInsert._id}), docToInsert);
            this.expectedCounters[docToInsert._id] = docToInsert.counter;
        };

        this.runInternalTransaction(
            db, collection, executionCtxType, insertCmdObj, checkResponseFunc, checkDocsFunc);
    };

    $config.states.internalTransactionForUpdate = function internalTransactionForUpdate(db,
                                                                                        collName) {
        const executionCtxType = this.generateRandomExecutionContext();
        db = this.getDB(db, executionCtxType);
        const collection = db.getCollection(collName);

        const {docToUpdate, updatedDoc, update} = this.generateRandomUpdate(collection);
        const updateCmdObj = {
            update: collection.getName(),
            updates: [{q: this.getQueryForDocument(collection, docToUpdate), u: update}]
        };
        const checkResponseFunc = (res) => {
            assert.eq(res.n, 1, res);
            assert.eq(res.nModified, 1, res);
        };
        const checkDocsFunc = () => {
            assert.isnull(collection.findOne(docToUpdate));
            assert.eq(collection.findOne(this.getQueryForDocument(collection, docToUpdate)),
                      updatedDoc);
            this.expectedCounters[docToUpdate._id] = updatedDoc.counter;
        };

        this.runInternalTransaction(
            db, collection, executionCtxType, updateCmdObj, checkResponseFunc, checkDocsFunc);
    };

    $config.states.internalTransactionForDelete = function internalTransactionForDelete(db,
                                                                                        collName) {
        const executionCtxType = this.generateRandomExecutionContext();
        db = this.getDB(db, executionCtxType);
        const collection = db.getCollection(collName);

        const docToDelete = this.generateRandomDelete(collection);
        const deleteCmdObj = {
            delete: collection.getName(),
            deletes: [{q: this.getQueryForDocument(collection, docToDelete), limit: 0}]
        };
        const checkResponseFunc = (res) => {
            assert.eq(res.n, 1, res);
        };
        const checkDocsFunc = () => {
            assert.isnull(collection.findOne(docToDelete));
            delete this.expectedCounters[docToDelete._id];
        };

        this.runInternalTransaction(
            db, collection, executionCtxType, deleteCmdObj, checkResponseFunc, checkDocsFunc);
    };

    $config.states.internalTransactionForFindAndModify =
        function internalTransactionForFindAndModify(db, collName) {
        const executionCtxType = this.generateRandomExecutionContext();
        db = this.getDB(db, executionCtxType);
        const collection = db.getCollection(collName);

        const {docToUpdate, updatedDoc, update, isUpsert, imageType} =
            this.generateRandomFindAndModify(collection);
        const findAndModifyCmdObj = {
            findAndModify: collection.getName(),
            query: this.getQueryForDocument(collection, docToUpdate),
            update: update
        };
        findAndModifyCmdObj.upsert = isUpsert;
        if (imageType == imageTypes.kPostImage) {
            findAndModifyCmdObj.new = true;
        }
        const checkResponseFunc = (res) => {
            assert.eq(res.lastErrorObject.n, 1, res);
            if (isUpsert) {
                assert.eq(res.lastErrorObject.updatedExisting, false, res);
                assert.eq(res.lastErrorObject.upserted, updatedDoc._id, res);
            } else {
                assert.eq(res.lastErrorObject.updatedExisting, true, res);
                assert.eq(
                    res.value, imageType == imageTypes.kPreImage ? docToUpdate : updatedDoc, res);
            }
        };
        const checkDocsFunc = () => {
            assert.isnull(collection.findOne(docToUpdate));
            assert.neq(collection.findOne(updatedDoc), null);
            this.expectedCounters[docToUpdate._id] = updatedDoc.counter;
        };

        this.runInternalTransaction(db,
                                    collection,
                                    executionCtxType,
                                    findAndModifyCmdObj,
                                    checkResponseFunc,
                                    checkDocsFunc);
    };

    /**
     * Asserts that the counter values for all documents assigned to this thread match their
     * expected values.
     */
    $config.states.verifyDocuments = function verifyDocuments(db, collName) {
        // The read below should not be done inside a transaction (and use readConcern level
        // "snapshot").
        fsm.forceRunningOutsideTransaction(this);

        // Run the find command with batch size equal to the number of documents + 1 to avoid
        // running getMore commands as getMore's are not retryable upon network errors.
        const numDocs = Object.keys(this.expectedCounters).length;
        const findCmdObj = {
            find: collName,
            filter: {tid: this.tid},
            batchSize: numDocs + 1,
        };
        if (this.shouldUseCausalConsistency) {
            findCmdObj.readConcern = {
                afterClusterTime: this.getMaxClusterTime(
                    [this.nonRetryableWriteSession, this.retryableWriteSession])
            };
            if (TestData.runningWithShardStepdowns) {
                findCmdObj.readConcern.level = "majority";
            }
        }
        const docs = assert.commandWorked(db.runCommand(findCmdObj)).cursor.firstBatch;
        assert.eq(docs.length, numDocs);
        docs.forEach(doc => {
            assert(doc._id in this.expectedCounters);
            const expectedCounter = this.expectedCounters[doc._id];
            assert.eq(expectedCounter, doc.counter, () => {
                return 'unexpected counter value, doc: ' + tojson(doc);
            });
        });
    };

    if ($config.passConnectionCache) {
        // If 'passConnectionCache' is true, every state function must accept 3 parameters: db,
        // collName and connCache. This workload does not set 'passConnectionCache' since it doesn't
        // use 'connCache' but it may extend a sharding workload that uses it.
        const originalInit = $config.states.init;
        $config.states.init = function(db, collName, connCache) {
            originalInit.call(this, db, collName);
        };

        const originalInternalTransactionForInsert = $config.states.internalTransactionForInsert;
        $config.states.internalTransactionForInsert = function(db, collName, connCache) {
            originalInternalTransactionForInsert.call(this, db, collName);
        };

        const originalInternalTransactionForUpdate = $config.states.internalTransactionForUpdate;
        $config.states.internalTransactionForUpdate = function(db, collName, connCache) {
            originalInternalTransactionForUpdate.call(this, db, collName);
        };

        const originalInternalTransactionForDelete = $config.states.internalTransactionForDelete;
        $config.states.internalTransactionForDelete = function(db, collName, connCache) {
            originalInternalTransactionForDelete.call(this, db, collName);
        };

        const originalInternalTransactionForFindAndModify =
            $config.states.internalTransactionForFindAndModify;
        $config.states.internalTransactionForFindAndModify = function(db, collName, connCache) {
            originalInternalTransactionForFindAndModify.call(this, db, collName);
        };

        const originalVerifyDocuments = $config.states.verifyDocuments;
        $config.states.verifyDocuments = function(db, collName, connCache) {
            originalVerifyDocuments.call(this, db, collName);
        };
    }

    $config.transitions = {
        init: {
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            internalTransactionForFindAndModify: 0.25,
        },
        internalTransactionForInsert: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2
        },
        internalTransactionForUpdate: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2
        },
        internalTransactionForDelete: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2
        },
        internalTransactionForFindAndModify: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2
        },
        verifyDocuments: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2
        }
    };

    return $config;
});
