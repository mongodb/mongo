'use strict';

/**
 * Creates several bank accounts. On each iteration, each thread:
 *  - chooses two accounts and amount of money being transfer
 *  - or checks the balance of each account
 *
 * @tags: [uses_transactions, assumes_snapshot_transactions]
 */

// For withTxnAndAutoRetry.
load('jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js');

var $config = (function() {
    function computeTotalOfAllBalances(documents) {
        return documents.reduce((total, account) => total + account.balance, 0);
    }

    var states = (function() {
        function getAllDocuments(session, collection, numDocs, txnHelperOptions) {
            let documents;
            withTxnAndAutoRetry(session, () => {
                documents = collection.find().toArray();

                assertWhenOwnColl.eq(numDocs, documents.length, () => tojson(documents));
            }, txnHelperOptions);
            return documents;
        }

        function init(db, collName) {
            this.session = db.getMongo().startSession({causalConsistency: true});
        }

        function checkMoneyBalance(db, collName) {
            const collection = this.session.getDatabase(db.getName()).getCollection(collName);
            const documents = getAllDocuments(this.session, collection, this.numAccounts, {
                retryOnKilledSession: this.retryOnKilledSession
            });
            assertWhenOwnColl.eq(this.numAccounts * this.initialValue,
                                 computeTotalOfAllBalances(documents),
                                 () => tojson(documents));
        }

        function transferMoney(db, collName) {
            const transferFrom = Random.randInt(this.numAccounts);
            let transferTo = Random.randInt(this.numAccounts);
            while (transferFrom === transferTo) {
                transferTo = Random.randInt(this.numAccounts);
            }

            // We make 'transferAmount' non-zero in order to guarantee that the documents matched by
            // the update operations are modified.
            const transferAmount = Random.randInt(this.initialValue / 10) + 1;

            const collection = this.session.getDatabase(db.getName()).getCollection(collName);
            withTxnAndAutoRetry(this.session, () => {
                let res = collection.runCommand('update', {
                    updates: [{q: {_id: transferFrom}, u: {$inc: {balance: -transferAmount}}}],
                });

                assertAlways.commandWorked(res);
                assertWhenOwnColl.eq(res.n, 1, () => tojson(res));
                assertWhenOwnColl.eq(res.nModified, 1, () => tojson(res));

                res = collection.runCommand('update', {
                    updates: [{q: {_id: transferTo}, u: {$inc: {balance: transferAmount}}}],
                });

                assertAlways.commandWorked(res);
                assertWhenOwnColl.eq(res.n, 1, () => tojson(res));
                assertWhenOwnColl.eq(res.nModified, 1, () => tojson(res));
            }, {retryOnKilledSession: this.retryOnKilledSession});
        }

        return {init: init, transferMoney: transferMoney, checkMoneyBalance: checkMoneyBalance};
    })();

    function setup(db, collName, cluster) {
        assertWhenOwnColl.commandWorked(db.runCommand({create: collName}));

        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numAccounts; ++i) {
            bulk.insert({_id: i, balance: this.initialValue});
        }

        const res = bulk.execute({w: 'majority'});
        assertWhenOwnColl.commandWorked(res);
        assertWhenOwnColl.eq(this.numAccounts, res.nInserted);

        if (cluster.isSharded()) {
            // Advance each router's cluster time to be >= the time of the writes, so the first
            // global snapshots chosen by each is guaranteed to include the inserted documents.
            cluster.synchronizeMongosClusterTimes();
        }
    }

    function teardown(db, collName, cluster) {
        const documents = db[collName].find().toArray();
        assertWhenOwnColl.eq(this.numAccounts * this.initialValue,
                             computeTotalOfAllBalances(documents),
                             () => tojson(documents));
    }

    var transitions = {
        init: {transferMoney: 1},
        transferMoney: {transferMoney: 0.9, checkMoneyBalance: 0.1},
        checkMoneyBalance: {transferMoney: 1}
    };

    return {
        threadCount: 10,
        iterations: 100,
        startState: 'init',
        states: states,
        transitions: transitions,
        data: {numAccounts: 20, initialValue: 2000, retryOnKilledSession: false},
        setup: setup,
        teardown: teardown
    };
})();
