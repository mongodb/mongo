'use strict';

/**
 * Performs repeated reads of the documents in the collection to test snapshot isolation.
 *
 * @tags: [uses_transactions, assumes_snapshot_transactions]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/multi_statement_transaction_simple.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.numReads = 5;

    $config.states.repeatedRead = function repeatedRead(db, collName) {
        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            let prevDocuments = undefined;
            for (let i = 0; i < this.numReads; i++) {
                const collectionDocs = collection.find().toArray();
                assertWhenOwnColl.eq(
                    this.numAccounts, collectionDocs.length, () => tojson(collectionDocs));
                if (prevDocuments) {
                    assertAlways.sameMembers(
                        prevDocuments,
                        collectionDocs,
                        () => "Document mismatch - previous documents: " +
                            tojsononeline(prevDocuments) +
                            ", current documents: " + tojsononeline(collectionDocs),
                        bsonBinaryEqual);  // Exact document matches.
                }
                prevDocuments = collectionDocs;
            }
        });
    };

    $config.transitions = {
        init: {transferMoney: 1},
        transferMoney: {transferMoney: 0.7, checkMoneyBalance: 0.1, repeatedRead: 0.2},
        checkMoneyBalance: {transferMoney: 1},
        repeatedRead: {transferMoney: 0.7, repeatedRead: 0.3},
    };

    return $config;
});
