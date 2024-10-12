'use strict';

/**
 * Runs insert, update, delete and findAndModify commands against a sharded collection inside
 * single-shard and cross-shard internal transactions started on a shard using all the available
 * client session settings. Only runs on sharded clusters.
 *
 * @tags: [
 *  requires_fcv_60,
 *  requires_sharding,
 *  uses_transactions,
 *  antithesis_incompatible
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/internal_transactions_sharded.js');
load('jstests/libs/fixture_helpers.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.data.expectDirtyDocs = {
        // The client is either not using a session or is using a session without retryable writes
        // enabled. Therefore, when a write is interrupted due to stepdown/kill/terminate, they
        // cannot retry the write to verify if it has been executed or not.
        [$super.data.executionContextTypes.kNoClientSession]: TestData.runningWithShardStepdowns,
        [$super.data.executionContextTypes.kClientSession]: TestData.runningWithShardStepdowns
    };

    $config.data.useClusterClient = true;

    $config.data.generateRandomExecutionContext = function generateRandomExecutionContext() {
        if (this.shouldUseCausalConsistency) {
            // Exclude kNoClientSession since a (casually consistent) session is required.
            return 2;
        }
        return this.generateRandomInt(1, 2);
    };

    $config.data.bumpClusterTime = function bumpClusterTime(db, collName) {
        // Make the mongos do an insert against both shards to make bump its clusterTime.
        const docs = [
            this.generateRandomDocument(this.tid, {isLowerChunkDoc: true}),
            this.generateRandomDocument(this.tid, {isUpperChunkDoc: true})
        ];
        docs.forEach(doc => {
            this.expectedCounters[doc._id] = doc.counter;
        });
        const session = db.getMongo().startSession({retryWrites: true});
        const collection = session.getDatabase(db.getName()).getCollection(collName);
        assert.commandWorked(collection.insert(docs));
    };

    $config.data.getCollectionForDocumentChecks = function getCollectionForDocumentChecks(
        defaultDb, txnDb, collName) {
        assert(isMongos(defaultDb));
        assert(!isMongos(txnDb));
        print(tojsononeline(
            {defaultHost: defaultDb.getMongo().host, txnHost: txnDb.getMongo().host}));
        // Bump the clusterTime on the mongos so that when causal consistency is required, the reads
        // performed by the document checks will have afterClusterTime greater than the timestamp of
        // the writes done by the transaction.
        this.bumpClusterTime(defaultDb, collName);
        return defaultDb.getCollection(collName);
    };

    $config.data.startSessions = function startSessions(db) {
        const randomShardPrimary = this.randomShardRst.getPrimary();

        this.mongo = randomShardPrimary;
        this.nonRetryableWriteSession = this.mongo.startSession(
            {causalConsistency: this.shouldUseCausalConsistency, retryWrites: false});
        this.retryableWriteSession = this.mongo.startSession(
            {causalConsistency: this.shouldUseCausalConsistency, retryWrites: true});
        if (this.sessions === undefined) {
            this.sessions = [];
        }
        this.sessions.push(this.nonRetryableWriteSession);
        this.sessions.push(this.retryableWriteSession);

        const randomShardInfo = {shard: this.randomShardRst.name, primary: randomShardPrimary};
        print(`Started a non-retryable write session ${
            tojsononeline(this.nonRetryableWriteSession.getSessionId())} against shard ${
            tojsononeline(randomShardInfo)}}`);
        print(`Started a retryable write session ${
            tojsononeline(this.retryableWriteSession.getSessionId())} against shard ${
            tojsononeline(randomShardInfo)}}`);
    };

    $config.data.runInternalTransaction = function runInternalTransaction(
        defaultDb, collName, executionCtxType, crudOp) {
        assert.neq(executionCtxType, this.executionContextTypes.kClientRetryableWrite);
        assert.neq(executionCtxType, this.executionContextTypes.kClientTransaction);

        try {
            $super.data.runInternalTransaction.apply(this, arguments);
        } catch (e) {
            if (isNetworkError(e) || isRetryableError(e)) {
                print("Starting new sessions after internal transaction error: " +
                      tojsononeline(e));
                this.startSessions(defaultDb);
                // When causal consistency is required, the verifyDocuments state would perform
                // reads against mongos with afterClusterTime equal to the max of the clusterTimes
                // of all sessions that it has created on the shard that it uses to run internal
                // transactions from. Bump the clusterTime on the mongos after the shard has
                // recovered so that the mongos can gossip the clusterTime correctly to the other
                // shard; otherwise when the next state is the verifyDocuments state, the
                // afterClusterTime in the command could be higher than the clusterTime known to
                // that shard and that would cause the command to fail.
                this.bumpClusterTime(defaultDb, collName);
                return;
            }
            throw e;
        }
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, arguments);

        // If a shard node that is acting as a router for an internal transaction is
        // killed/terminated/stepped down or the transaction's session is killed while running a
        // non-retryable transaction, the transaction would be left in-progress since nothing
        // would aborted it. Such dangling transactions can cause the CheckReplDBHash hook to hang
        // as the fsyncLock command requires taking the global S lock and it cannot do that while
        // there is an in-progress transaction.
        if (TestData.runningWithShardStepdowns || this.retryOnKilledSession) {
            this.killAllSessions(cluster);
        }
    };

    $config.states.init = function init(db, collName, connCache) {
        const retryableErrorMessages = [
            "The server is in quiesce mode and will shut down",
            "can't connect to new replica set primary"
        ];

        let shardRsts;
        while (true) {
            try {
                shardRsts = FixtureHelpers.getAllReplicas(db);
                break;
            } catch (e) {
                if (retryableErrorMessages.some(msg => e.message.includes(msg))) {
                    print("Retry getting all shard replica sets after error: " + tojson(e));
                    continue;
                }
                throw e;
            }
        }
        this.randomShardRst = shardRsts[Random.randInt(shardRsts.length)];

        $super.states.init.apply(this, arguments);
    };

    $config.transitions = {
        init: {
            internalTransactionForInsert: 0.4,
            internalTransactionForUpdate: 0.3,
            internalTransactionForDelete: 0.3,
        },
        internalTransactionForInsert: {
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            verifyDocuments: 0.25
        },
        internalTransactionForUpdate: {
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            verifyDocuments: 0.25
        },
        internalTransactionForDelete: {
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            verifyDocuments: 0.25
        },
        internalTransactionForFindAndModify: {
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            verifyDocuments: 0.25
        },
        verifyDocuments: {
            internalTransactionForInsert: 0.4,
            internalTransactionForUpdate: 0.3,
            internalTransactionForDelete: 0.3,
        }
    };

    return $config;
});
