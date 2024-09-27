/**
 * Transactionally updates a per thread counter in documents written to by the base workload and
 * reads the value of a random, possibly the same, counter to verify it matches the expected value.
 * This is meant to exercise the single write shard commit optimization for sharded clusters.
 *
 * The base workload assumes transactions run at snapshot read concern, so the tag is copied here.
 *
 * @tags: [
 *    uses_transactions,
 *    assumes_snapshot_transactions,
 *    config_shard_incompatible
 * ]
 */

// Our concurrency suites don't crash routers, so even in failover suites a router will rarely run
// out of retries and require a shell level retry, so force retrying writes at least once to get
// better coverage for the commitTransaction retry bug described in SERVER-48307.
import "jstests/libs/override_methods/retry_writes_at_least_once.js";

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    withTxnAndAutoRetry
} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/txns/multi_statement_transaction/multi_statement_transaction_simple.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.counters = {};
    $config.data.threadUniqueField = "";

    // Returns two random document ids from the base workload, which inserted documents with _ids
    // ranging from 0 to the number of accounts.
    function getDocIds(numAccounts) {
        return {writeDocId: Random.randInt(numAccounts), readDocId: Random.randInt(numAccounts)};
    }

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        // Each thread updates a thread unique field to avoid collisions.
        this.threadUniqueField = 'thread' + this.tid;
        for (let i = 0; i < this.numAccounts; i++) {
            this.counters[i] = 0;
        }
    };

    $config.states.writeThenReadTxn = function writeThenReadTxn(db, collName) {
        const {writeDocId, readDocId} = getDocIds(this.numAccounts);
        this.counters[writeDocId] += 1;

        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            let res = collection.runCommand('update', {
                updates: [{q: {_id: writeDocId}, u: {$inc: {[this.threadUniqueField]: 1}}}],
            });

            assert.commandWorked(res);
            assert.eq(res.n, 1, () => tojson(res));
            assert.eq(res.nModified, 1, () => tojson(res));

            const doc = collection.findOne({_id: readDocId});
            assert.eq(
                // The document has been updated, so the in-memory counters should all be correct.
                this.counters[readDocId],
                // We don't initialize the thread unique field to 0, so treat undefined as 0.
                doc.hasOwnProperty(this.threadUniqueField) ? doc[this.threadUniqueField] : 0,
                'unexpected thread unique field value in transaction, read after write, thread: ' +
                    this.tid + ', readId: ' + readDocId + ', writeId: ' + writeDocId +
                    ', doc: ' + tojson(doc));
        }, {retryOnKilledSession: this.retryOnKilledSession});
    };

    $config.states.readThenWriteTxn = function readThenWriteTxn(db, collName) {
        const {writeDocId, readDocId} = getDocIds(this.numAccounts);
        this.counters[writeDocId] += 1;

        // The in-memory counter is updated before writing to the database, so if the read doc is
        // the write doc, we should expect one less than the in-memory counter for the read before
        // the first write.
        const expectedCounterBeforeWrite =
            readDocId !== writeDocId ? this.counters[readDocId] : this.counters[readDocId] - 1;

        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            const doc = collection.findOne({_id: readDocId});
            assert.eq(
                expectedCounterBeforeWrite,
                // We don't initialize the thread unique field to 0, so treat undefined as 0.
                doc.hasOwnProperty(this.threadUniqueField) ? doc[this.threadUniqueField] : 0,
                'unexpected thread unique field value in transaction, read before write, thread: ' +
                    this.tid + ', readId: ' + readDocId + ', writeId: ' + writeDocId +
                    ', doc: ' + tojson(doc));

            const updateRes = collection.runCommand('update', {
                updates: [{q: {_id: writeDocId}, u: {$inc: {[this.threadUniqueField]: 1}}}],
            });

            assert.commandWorked(updateRes);
            assert.eq(updateRes.n, 1, () => tojson(updateRes));
            assert.eq(updateRes.nModified, 1, () => tojson(updateRes));
        }, {retryOnKilledSession: this.retryOnKilledSession});
    };

    $config.states.readThenWriteThenReadTxn = function readThenWriteThenReadTxn(db, collName) {
        const {writeDocId, readDocId} = getDocIds(this.numAccounts);
        this.counters[writeDocId] += 1;

        // The in-memory counter is updated before writing to the database, so if the read doc is
        // the write doc, we should expect one less than the in-memory counter for the read before
        // the first write.
        const expectedCounterBeforeWrite =
            readDocId !== writeDocId ? this.counters[readDocId] : this.counters[readDocId] - 1;

        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            let doc = collection.findOne({_id: readDocId});
            assert.eq(
                expectedCounterBeforeWrite,
                // We don't initialize the thread unique field to 0, so treat undefined as 0.
                doc.hasOwnProperty(this.threadUniqueField) ? doc[this.threadUniqueField] : 0,
                'unexpected thread unique field value in transaction, read before write, thread: ' +
                    this.tid + ', readId: ' + readDocId + ', writeId: ' + writeDocId +
                    ', doc: ' + tojson(doc));

            const updateRes = collection.runCommand('update', {
                updates: [{q: {_id: writeDocId}, u: {$inc: {[this.threadUniqueField]: 1}}}],
            });

            assert.commandWorked(updateRes);
            assert.eq(updateRes.n, 1, () => tojson(updateRes));
            assert.eq(updateRes.nModified, 1, () => tojson(updateRes));

            doc = collection.findOne({_id: readDocId});
            assert.eq(
                // The document has been updated, so the in-memory counters should all be correct.
                this.counters[readDocId],
                // We don't initialize the thread unique field to 0, so treat undefined as 0.
                doc.hasOwnProperty(this.threadUniqueField) ? doc[this.threadUniqueField] : 0,
                'unexpected thread unique field value in transaction, read after write, thread: ' +
                    this.tid + ', readId: ' + readDocId + ', writeId: ' + writeDocId +
                    ', doc: ' + tojson(doc));
        }, {retryOnKilledSession: this.retryOnKilledSession});
    };

    $config.transitions = {
        init: {transferMoney: 1},
        transferMoney: {
            transferMoney: 0.2,
            checkMoneyBalance: 0.2,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
        },
        checkMoneyBalance: {
            transferMoney: 0.4,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
        },
        writeThenReadTxn: {
            transferMoney: 0.2,
            checkMoneyBalance: 0.2,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
        },
        readThenWriteTxn: {
            transferMoney: 0.2,
            checkMoneyBalance: 0.2,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
        },
        readThenWriteThenReadTxn: {
            transferMoney: 0.2,
            checkMoneyBalance: 0.2,
            writeThenReadTxn: 0.2,
            readThenWriteTxn: 0.2,
            readThenWriteThenReadTxn: 0.2,
        },
    };

    return $config;
});
