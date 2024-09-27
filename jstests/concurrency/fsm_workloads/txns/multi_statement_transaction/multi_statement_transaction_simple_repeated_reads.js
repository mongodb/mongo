/**
 * Performs repeated reads of the documents in the collection to test snapshot isolation.
 *
 * @tags: [uses_transactions, assumes_snapshot_transactions]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    withTxnAndAutoRetry
} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/txns/multi_statement_transaction/multi_statement_transaction_simple.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.numReads = 5;

    $config.states.repeatedRead = function repeatedRead(db, collName) {
        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            let prevDocuments = undefined;
            for (let i = 0; i < this.numReads; i++) {
                const collectionDocs = collection.find().toArray();
                assert.eq(this.numAccounts, collectionDocs.length, () => tojson(collectionDocs));
                if (prevDocuments) {
                    assert.sameMembers(prevDocuments,
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
