/**
 *  Performs repeated reads of the documents in the collection to test snapshot isolation.
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
    "jstests/concurrency/fsm_workloads/txns/multi_statement_transaction/multi_statement_transaction_atomicity_isolation.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.numReads = 5;

    $config.states.repeatedRead = function repeatedRead(db, collName) {
        // We intentionally use a smaller batch size when fetching all of the documents in the
        // collection in order to stress the behavior of reading from the same snapshot over the
        // course of multiple network roundtrips.
        const batchSize = Math.max(2, Math.floor(this.numDocs / 5));
        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            let prevDocuments = undefined;
            for (let i = 0; i < this.numReads; i++) {
                const collectionDocs = collection.find().batchSize(batchSize).toArray();
                assert.eq(this.numDocs, collectionDocs.length, () => {
                    return "txnNumber: " + tojson(this.session.getTxnNumber_forTesting()) +
                        ", session id: " + tojson(this.session.getSessionId()) +
                        ", read number: " + i + ", collection docs: " + tojson(collectionDocs);
                });
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
        init: {update: 0.7, checkConsistency: 0.1, repeatedRead: 0.2},
        update: {update: 0.7, checkConsistency: 0.1, repeatedRead: 0.2},
        checkConsistency: {update: 0.8, repeatedRead: 0.2},
        repeatedRead: {update: 0.8, repeatedRead: 0.2}
    };

    return $config;
});
