'use strict';

/**
 *  Performs repeated reads of the documents in the collection to test snapshot isolation.
 *
 * @tags: [uses_transactions]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/multi_statement_transaction_atomicity_isolation.js');
// for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.numReads = 5;

    $config.states.repeatedRead = function repeatedRead(db, collName) {
        // We intentionally use a smaller batch size when fetching all of the documents in the
        // collection in order to stress the behavior of reading from the same snapshot over the
        // course of multiple network roundtrips.
        const batchSize = Math.max(2, Math.floor(this.numDocs / 5));
        let prevDocuments;
        const collection = this.session.getDatabase(db.getName()).getCollection(collName);
        withTxnAndAutoRetry(this.session, () => {
            for (let i = 0; i < this.numReads; i++) {
                const collectionDocs = collection.find().batchSize(batchSize).toArray();
                assertWhenOwnColl.eq(
                    this.numDocs, collectionDocs.length, () => tojson(collectionDocs));
                if (prevDocuments) {
                    assertAlways.eq(prevDocuments.length, collectionDocs.length);
                    for (let j = 0; j < prevDocuments.length; j++) {
                        assertAlways(bsonBinaryEqual(prevDocuments[j], collectionDocs[j]),
                                     () => "Document mismatch for read " + i + " document index " +
                                         j + " Previous documents: " +
                                         tojsononeline(prevDocuments) + " Current documents: " +
                                         tojsononeline(collectionDocs));
                    }
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
