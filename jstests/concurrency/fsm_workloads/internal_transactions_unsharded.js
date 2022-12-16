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
 *  uses_transactions,
 *  assumes_unsharded_collection,
 *  # The default linearizable readConcern timeout is too low and may cause tests to fail.
 *  does_not_support_config_fuzzer,
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

    $config.data.executionContextTypes =
        {kNoClientSession: 1, kClientSession: 2, kClientRetryableWrite: 3, kClientTransaction: 4};
    $config.data.imageTypes = {kPreImage: 1, kPostImage: 2};

    // The number of documents assigned to a thread when the workload starts.
    $config.data.partitionSize = 200;
    // The batch size for the find command used for looking up the documents assigned to a thread.
    // Use a large batch size so that a getMore command is never needed since getMore is not
    // retryable after network errors.
    $config.data.batchSizeForDocsLookUp = 1000;
    // The counter values for the documents assigned to a thread. The map is populated during
    // the init state and is updated after every write in the other states. Used to verify that
    // updates aren't double applied.
    $config.data.expectedCounters = {};
    // Keep track of the documents that a thread has started writing to but does not know if the
    // write has succeeded, for example, because the write is interrupted. The key of the inner map
    // is the document id, and the value is command name for the write against that document (i.e.
    // "insert", "update", "delete" or "findAndModify").
    $config.data.dirtyDocs = {
        [$config.data.executionContextTypes.kNoClientSession]: {},
        [$config.data.executionContextTypes.kClientSession]: {},
        [$config.data.executionContextTypes.kClientRetryableWrite]: {},
        [$config.data.executionContextTypes.kClientTransaction]: {},
    };
    $config.data.expectDirtyDocs = {
        [$config.data.executionContextTypes.kNoClientSession]: false,
        [$config.data.executionContextTypes.kClientSession]: false,
        [$config.data.executionContextTypes.kClientRetryableWrite]: false,
        [$config.data.executionContextTypes.kClientTransaction]: false,
    };

    // The reap threshold is overriden to get coverage for when it schedules reaps during an active
    // workload.
    $config.data.originalInternalSessionReapThreshold = {};

    // This workload supports setting the 'transactionLifetimeLimitSeconds' to 45 seconds
    // (configurable) during setup() and restoring the original value during teardown().
    $config.data.lowerTransactionLifetimeLimitSeconds = false;
    $config.data.transactionLifetimeLimitSeconds = 45;
    $config.data.originalTransactionLifetimeLimitSeconds = {};

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

    $config.data.startSessions = function startSessions(db) {
        this.mongo = db.getMongo();
        this.nonRetryableWriteSession = this.mongo.startSession(
            {causalConsistency: this.shouldUseCausalConsistency, retryWrites: false});
        this.retryableWriteSession = this.mongo.startSession(
            {causalConsistency: this.shouldUseCausalConsistency, retryWrites: true});
        this.sessions = [this.nonRetryableWriteSession, this.retryableWriteSession];

        print(`Started a non-retryable write session ${
            tojsononeline(this.nonRetryableWriteSession.getSessionId())}`);
        print(`Started a retryable write session ${
            tojsononeline(this.retryableWriteSession.getSessionId())}`);
    };

    $config.data.getInternalTransactionDB = function getDB(executionCtxType, dbName) {
        switch (executionCtxType) {
            case this.executionContextTypes.kNoClientSession:
                return this.mongo.getDB(dbName);
            case this.executionContextTypes.kClientSession:
                return this.nonRetryableWriteSession.getDatabase(dbName);
            case this.executionContextTypes.kClientRetryableWrite:
                return this.retryableWriteSession.getDatabase(dbName);
            case this.executionContextTypes.kClientTransaction:
                return this.generateRandomBool() ? this.nonRetryableWriteSession.getDatabase(dbName)
                                                 : this.retryableWriteSession.getDatabase(dbName);
            default:
                throw Error("Unknown execution context");
        }
    };

    $config.data.getCollectionForDocumentChecks = function getCollectionForDocumentChecks(
        defaultDb, txnDb, collName) {
        assert.eq(defaultDb.getMongo().host, txnDb.getMongo().host);
        return txnDb.getCollection(collName);
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

    $config.data.isDirtyDocument = function isDirtyDocument(doc) {
        for (const executionCtxType in this.dirtyDocs) {
            if (doc._id in this.dirtyDocs[executionCtxType]) {
                return true;
            }
        }
        return false;
    };

    $config.data.getQueryForDocument = function getQueryForDocument(doc) {
        return {_id: doc._id, tid: this.tid};
    };

    /**
     * Returns true if 'res' contains an acceptable error for the aggregate command used to look up
     * a random document.
     */
    $config.data.isAcceptableAggregateCmdError = function isAcceptableAggregateCmdError(res) {
        // The aggregate command is expected to involve running getMore commands which are not
        // retryable after network errors.
        return TestData.runningWithShardStepdowns && res &&
            (res.code == ErrorCodes.QueryPlanKilled);
    };

    $config.data.getRandomDocument = function getRandomDocument(db, collName) {
        const aggregateCmdObj = {
            aggregate: collName,
            cursor: {},
            pipeline: [{$match: {tid: this.tid}}, {$sample: {size: 1}}],
        };
        // Use linearizable read concern to guarantee any subsequent transaction snapshot will
        // include the found document. Skip if the test has a default read concern or requires
        // casual consistency because in both cases the default read concern should provide this
        // guarantee already.
        if (!TestData.defaultReadConcernLevel &&
            !db.getSession().getOptions().isCausalConsistency()) {
            aggregateCmdObj.readConcern = {level: "linearizable"};
        }

        let numTries = 0;
        const numDocs = Object.keys(this.expectedCounters).length;
        while (numTries < numDocs) {
            print("Finding a random document " +
                  tojsononeline({aggregateCmdObj, numTries, numDocs}));
            let aggRes;
            assert.soon(() => {
                try {
                    aggRes = db.runCommand(aggregateCmdObj);
                    assert.commandWorked(aggRes);
                    return true;
                } catch (e) {
                    if (this.isAcceptableAggregateCmdError(aggRes)) {
                        return false;
                    }
                    throw e;
                }
            });
            const doc = aggRes.cursor.firstBatch[0];
            print("Found a random document " +
                  tojsononeline({doc, isDirty: this.isDirtyDocument(doc)}));
            if (!this.isDirtyDocument(doc)) {
                return doc;
            }
            numTries++;
        }
        throw Error("Could not find a clean document");
    };

    $config.data.generateRandomDocument = function generateRandomDocument(tid) {
        return {_id: UUID(), tid: tid, counter: 0};
    };

    $config.data.generateRandomInsert = function generateRandomInsert(db, collName) {
        const docToInsert = this.generateRandomDocument(this.tid);
        docToInsert[insertOpFieldName] = 0;

        const cmdObj = {insert: collName, documents: [docToInsert]};
        const checkResponseFunc = (res) => {
            assert.eq(res.n, 1, res);
        };
        const checkDocsFunc = (collection) => {
            assert.eq(collection.findOne({_id: docToInsert._id}), docToInsert);
            this.expectedCounters[docToInsert._id] = docToInsert.counter;
        };
        const docId = docToInsert._id;

        return {cmdObj, checkResponseFunc, checkDocsFunc, docId};
    };

    $config.data.generateRandomUpdate = function generateRandomUpdate(db, collName) {
        const docToUpdate = this.getRandomDocument(db, collName);
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

        const cmdObj = {
            update: collName,
            updates: [{q: this.getQueryForDocument(docToUpdate), u: update}]
        };
        const checkResponseFunc = (res) => {
            assert.eq(res.n, 1, res);
            assert.eq(res.nModified, 1, res);
        };
        const checkDocsFunc = (collection) => {
            assert.isnull(collection.findOne(docToUpdate));
            assert.eq(collection.findOne(this.getQueryForDocument(docToUpdate)), updatedDoc);
            this.expectedCounters[docToUpdate._id] = updatedDoc.counter;
        };
        const docId = docToUpdate._id;

        return {cmdObj, checkResponseFunc, checkDocsFunc, docId};
    };

    $config.data.generateRandomDelete = function generateRandomDelete(db, collName) {
        const docToDelete = this.getRandomDocument(db, collName);
        assert.neq(docToDelete, null);

        const cmdObj = {
            delete: collName,
            deletes: [{q: this.getQueryForDocument(docToDelete), limit: 1}]
        };
        const checkResponseFunc = (res) => {
            assert.eq(res.n, 1, res);
        };
        const checkDocsFunc = (collection) => {
            assert.isnull(collection.findOne(docToDelete));
            delete this.expectedCounters[docToDelete._id];
        };
        const docId = docToDelete._id;

        return {cmdObj, checkResponseFunc, checkDocsFunc, docId};
    };

    $config.data.generateRandomFindAndModify = function generateRandomFindAndModify(db, collName) {
        const isUpsert = this.generateRandomBool();
        const imageType = this.generateRandomImageType();

        const docToUpdate =
            isUpsert ? this.generateRandomDocument(this.tid) : this.getRandomDocument(db, collName);

        const updatedDoc = Object.assign({}, docToUpdate);
        updatedDoc[findAndModifyOpFieldName] = docToUpdate.hasOwnProperty(findAndModifyOpFieldName)
            ? (docToUpdate[findAndModifyOpFieldName] + 1)
            : 0;
        updatedDoc.counter += 1;

        const update = {
            $set: {[findAndModifyOpFieldName]: updatedDoc[findAndModifyOpFieldName]},
            $inc: {counter: 1}
        };

        const cmdObj = {
            findAndModify: collName,
            query: this.getQueryForDocument(docToUpdate),
            update: update
        };
        cmdObj.upsert = isUpsert;
        if (imageType == this.imageTypes.kPostImage) {
            cmdObj.new = true;
        }
        const checkResponseFunc = (res) => {
            assert.eq(res.lastErrorObject.n, 1, res);
            if (isUpsert) {
                assert.eq(res.lastErrorObject.updatedExisting, false, res);
                assert.eq(res.lastErrorObject.upserted, updatedDoc._id, res);
            } else {
                assert.eq(res.lastErrorObject.updatedExisting, true, res);
                assert.eq(res.value,
                          imageType == this.imageTypes.kPreImage ? docToUpdate : updatedDoc,
                          res);
            }
        };
        const checkDocsFunc = (collection) => {
            assert.isnull(collection.findOne(docToUpdate));
            assert.neq(collection.findOne(updatedDoc), null);
            this.expectedCounters[docToUpdate._id] = updatedDoc.counter;
        };
        const docId = docToUpdate._id;

        return {cmdObj, checkResponseFunc, checkDocsFunc, docId};
    };

    /**
     * Returns true if 'res' contains an acceptable retry error for a retryable write command.
     */
    $config.data.isAcceptableRetryError = function isAcceptableRetryError(res) {
        // This workload does not involve data placement changes so retries should always succeed.
        // Workloads that extend this workload should override this method accordingly.
        return false;
    };

    /**
     * Runs the command specified by 'crudOp.cmdObj' inside an internal transaction using the
     * specified client 'executionCtxType'.
     */
    $config.data.runInternalTransaction = function runInternalTransaction(
        defaultDb, collName, executionCtxType, crudOp) {
        // The testInternalTransactions command below runs with the session setting defined by
        // 'executionCtxType'.
        fsm.forceRunningOutsideTransaction(this);

        // Add an insert command to each transaction so that when this workload is running on a
        // sharded cluster there can be a mix of single-shard and cross-shard transactions.
        const insertOp = this.generateRandomInsert(defaultDb, collName);

        if (executionCtxType == this.executionContextTypes.kClientRetryableWrite) {
            crudOp.cmdObj.stmtId = NumberInt(1);
            insertOp.cmdObj.stmtId = NumberInt(-1);
        }
        const internalTxnTestCmdObj = {
            testInternalTransactions: 1,
            commandInfos: [
                {dbName: defaultDb.getName(), command: crudOp.cmdObj},
                {dbName: defaultDb.getName(), command: insertOp.cmdObj}
            ],
        };
        if (this.useClusterClient) {
            internalTxnTestCmdObj.useClusterClient = true;
        }

        print(`Running an internal transaction using a test command ${
            tojsononeline(internalTxnTestCmdObj)}: ${tojsononeline({executionCtxType})}`);
        const txnDb = this.getInternalTransactionDB(executionCtxType, defaultDb.getName());

        const runFunc = () => {
            let res;
            try {
                res = txnDb.adminCommand(internalTxnTestCmdObj);
                print(`Response: ${tojsononeline(res)}`);
                assert.commandWorked(res);
            } catch (e) {
                if ((executionCtxType == this.executionContextTypes.kClientRetryableWrite) &&
                    this.isAcceptableRetryError(res, executionCtxType)) {
                    print("Ignoring retry error for retryable write: " + tojsononeline(res));
                    return;
                }
                throw e;
            }

            // Check responses.
            res.responses.forEach(innerRes => {
                assert.commandWorked(innerRes);
            });
            if (executionCtxType == this.executionContextTypes.kClientRetryableWrite) {
                // If the command was retried, 'responses' would only contain the response for
                // 'crudOp.cmdObj'.
                assert.lte(res.responses.length, 2);
            } else {
                assert.eq(res.responses.length, 2);
            }
            const crudRes = res.responses[0];
            crudOp.checkResponseFunc(crudRes);
            if (res.responses.length == 2) {
                const insertRes = res.responses[1];
                insertOp.checkResponseFunc(insertRes);
            }
        };

        print("Starting internal transaction");
        this.dirtyDocs[executionCtxType][crudOp.docId] = Object.keys(crudOp.cmdObj)[0];
        this.dirtyDocs[executionCtxType][insertOp.docId] = "insert";

        if (executionCtxType == this.executionContextTypes.kClientTransaction) {
            withTxnAndAutoRetry(
                txnDb.getSession(), runFunc, {retryOnKilledSession: this.retryOnKilledSession});
        } else {
            runFunc();
        }

        // Check documents.
        const collection = this.getCollectionForDocumentChecks(defaultDb, txnDb, collName);
        crudOp.checkDocsFunc(collection);
        insertOp.checkDocsFunc(collection);

        delete this.dirtyDocs[executionCtxType][crudOp.docId];
        delete this.dirtyDocs[executionCtxType][insertOp.docId];
        print("Finished internal transaction");
    };

    $config.data.insertInitialDocuments = function insertInitialDocuments(db, collName, tid) {
        let bulk = db.getCollection(collName).initializeUnorderedBulkOp();
        for (let i = 0; i < this.partitionSize; ++i) {
            const doc = this.generateRandomDocument(tid);
            bulk.insert(doc);
        }
        assert.commandWorked(bulk.execute());
    };

    $config.data.overrideInternalTransactionsReapThreshold =
        function overrideInternalTransactionsReapThreshold(cluster) {
        const newThreshold = this.generateRandomInt(0, 4);
        print("Setting internalSessionsReapThreshold to " + newThreshold);
        cluster.executeOnMongodNodes((db) => {
            const res = assert.commandWorked(
                db.adminCommand({setParameter: 1, internalSessionsReapThreshold: newThreshold}));
            this.originalInternalSessionReapThreshold[db.getMongo().host] = res.was;
        });
    };

    $config.data.restoreInternalTransactionsReapThreshold =
        function restoreInternalTransactionsReapThreshold(cluster) {
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalSessionsReapThreshold:
                    this.originalInternalSessionReapThreshold[db.getMongo().host]
            }));
        });
    };

    $config.data.overrideTransactionLifetimeLimit = function overrideTransactionLifetimeLimit(
        cluster) {
        cluster.executeOnMongodNodes((db) => {
            const res = assert.commandWorked(db.adminCommand({
                setParameter: 1,
                transactionLifetimeLimitSeconds: this.transactionLifetimeLimitSeconds
            }));
            this.originalTransactionLifetimeLimitSeconds[db.getMongo().host] = res.was;
        });
    };

    $config.data.restoreTransactionLifetimeLimit = function restoreTransactionLifetimeLimit(
        cluster) {
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                transactionLifetimeLimitSeconds:
                    this.originalTransactionLifetimeLimitSeconds[db.getMongo().host]
            }));
        });
    };

    $config.data.killAllSessions = function killAllSessions(cluster) {
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand({killAllSessions: []}));
        });
    };

    $config.setup = function setup(db, collName, cluster) {
        assert.commandWorked(db.createCollection(collName, {writeConcern: {w: "majority"}}));
        if (this.insertInitialDocsOnSetUp) {
            // There isn't a way to determine what the thread ids are in setup phase so just assume
            // that they are [0, 1, ..., this.threadCount-1].
            for (let tid = 0; tid < this.threadCount; ++tid) {
                this.insertInitialDocuments(db, collName, tid);
            }
        }
        this.overrideInternalTransactionsReapThreshold(cluster);
        if (this.lowerTransactionLifetimeLimitSeconds) {
            this.overrideTransactionLifetimeLimit(cluster);
        }
    };

    $config.teardown = function teardown(db, collName, cluster) {
        this.restoreInternalTransactionsReapThreshold(cluster);
        if (this.lowerTransactionLifetimeLimitSeconds) {
            this.restoreTransactionLifetimeLimit(cluster);
        }
    };

    /**
     * Starts a retryable-write session and non-retryable write session, inserts the documents for
     * this thread, and populates the 'expectedCounters' map.
     */
    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        if (!this.insertInitialDocsOnSetUp) {
            this.insertInitialDocuments(db, collName, this.tid);
        }

        const docs = assert
                         .commandWorked(db.runCommand({
                             find: collName,
                             filter: {tid: this.tid},
                             batchSize: this.batchSizeForDocsLookUp,
                         }))
                         .cursor.firstBatch;
        assert.eq(docs.length, this.partitionSize);
        docs.forEach(doc => {
            this.expectedCounters[doc._id] = doc.counter;
        });

        this.startSessions(db);
        this.initTime = new Date();
    };

    $config.states.internalTransactionForInsert = function internalTransactionForInsert(db,
                                                                                        collName) {
        print("Starting internalTransactionForInsert");
        const executionCtxType = this.generateRandomExecutionContext();
        const insertOp = this.generateRandomInsert(db, collName);
        this.runInternalTransaction(db, collName, executionCtxType, insertOp);
        print("Finished internalTransactionForInsert");
    };

    $config.states.internalTransactionForUpdate = function internalTransactionForUpdate(db,
                                                                                        collName) {
        print("Starting internalTransactionForUpdate");
        const executionCtxType = this.generateRandomExecutionContext();
        const updateOp = this.generateRandomUpdate(db, collName);
        this.runInternalTransaction(db, collName, executionCtxType, updateOp);
        print("Finished internalTransactionForUpdate");
    };

    $config.states.internalTransactionForDelete = function internalTransactionForDelete(db,
                                                                                        collName) {
        print("Starting internalTransactionForDelete");
        const executionCtxType = this.generateRandomExecutionContext();
        const deleteOp = this.generateRandomDelete(db, collName);
        this.runInternalTransaction(db, collName, executionCtxType, deleteOp);
        print("Finished internalTransactionForDelete");
    };

    $config.states.internalTransactionForFindAndModify =
        function internalTransactionForFindAndModify(db, collName) {
        print("Starting internalTransactionForFindAndModify");
        const executionCtxType = this.generateRandomExecutionContext();
        const findAndModifyOp = this.generateRandomFindAndModify(db, collName);
        this.runInternalTransaction(db, collName, executionCtxType, findAndModifyOp);
        print("Finished internalTransactionForFindAndModify");
    };

    /**
     * Asserts that the counter values for all documents assigned to this thread match their
     * expected values.
     */
    $config.states.verifyDocuments = function verifyDocuments(db, collName) {
        print("Starting verifyDocuments");

        for (const executionCtxType in this.expectDirtyDocs) {
            const numDirtyDocs = Object.keys(this.dirtyDocs[executionCtxType]).length;
            print(`Dirty documents: ${tojsononeline({
                executionCtxType,
                count: numDirtyDocs,
                docs: this.dirtyDocs[executionCtxType]
            })}`);
            if (!this.expectDirtyDocs[executionCtxType]) {
                assert.eq(0,
                          numDirtyDocs,
                          () => `expected to find no dirty documents for ${
                              tojsononeline({executionCtxType})} but found ${numDirtyDocs}`);
            }
        }

        // The read below should not be done inside a transaction (and use readConcern level
        // "snapshot").
        fsm.forceRunningOutsideTransaction(this);

        const numDocsExpected = Object.keys(this.expectedCounters).length;
        const findCmdObj = {
            find: collName,
            filter: {tid: this.tid},
            batchSize: this.batchSizeForDocsLookUp,
        };
        if (this.shouldUseCausalConsistency) {
            findCmdObj.readConcern = {afterClusterTime: this.getMaxClusterTime(this.sessions)};
            if (TestData.runningWithShardStepdowns) {
                findCmdObj.readConcern.level = "majority";
            }
        }
        const docs = assert.commandWorked(db.runCommand(findCmdObj)).cursor.firstBatch;
        print("verifyDocuments " +
              tojsononeline(
                  {findCmdObj, numDocsFound: docs.length, numDocsExpected: numDocsExpected}));

        docs.forEach(doc => {
            if (this.isDirtyDocument(doc)) {
                return;
            }
            assert(doc._id in this.expectedCounters, tojson(doc));
            const expectedCounter = this.expectedCounters[doc._id];
            assert.eq(expectedCounter, doc.counter, () => {
                return 'unexpected counter value, doc: ' + tojson(doc);
            });
        });

        print("Finished verifyDocuments");
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
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            internalTransactionForFindAndModify: 0.25,
        }
    };

    return $config;
});
