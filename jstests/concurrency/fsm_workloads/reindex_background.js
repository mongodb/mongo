/**
 * reindex_background.js
 *
 * Bulk inserts 1000 documents and builds indexes in background, then alternates between reindexing
 * and querying against the collection. Operates on a separate collection for each thread. Note
 * that because indexes are initially built in the background, reindexing is also done in the
 * background.
 *
 * @tags: [SERVER-40561, creates_background_indexes, requires_getmore]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    assertWorkedHandleTxnErrors
} from "jstests/concurrency/fsm_workload_helpers/assert_handle_fail_in_transaction.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/reindex.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.prefix = 'reindex_background';

    $config.states.createIndexes = function createIndexes(db, collName) {
        const coll = db[this.threadCollName];

        // The number of indexes created here is also stored in data.nIndexes.
        assertWorkedHandleTxnErrors(coll.createIndex({text: 'text'}),
                                    ErrorCodes.IndexBuildAlreadyInProgress);
        assertWorkedHandleTxnErrors(coll.createIndex({geo: '2dsphere'}),
                                    ErrorCodes.IndexBuildAlreadyInProgress);
        assertWorkedHandleTxnErrors(coll.createIndex({integer: 1}),
                                    ErrorCodes.IndexBuildAlreadyInProgress);
        assertWorkedHandleTxnErrors(coll.createIndex({"$**": 1}),
                                    ErrorCodes.IndexBuildAlreadyInProgress);
    };

    return $config;
});
