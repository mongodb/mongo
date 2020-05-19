'use strict';

/**
 * reindex_background.js
 *
 * Bulk inserts 1000 documents and builds indexes in background, then alternates between reindexing
 * and querying against the collection. Operates on a separate collection for each thread. Note
 * that because indexes are initially built in the background, reindexing is also done in the
 * background.
 *
 * SERVER-36709: Disabled for ephemeralForTest due to excessive memory usage
 * @tags: [SERVER-40561, creates_background_indexes, incompatible_with_eft]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/reindex.js');     // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.prefix = 'reindex_background';

    $config.states.createIndexes = function createIndexes(db, collName) {
        const coll = db[this.threadCollName];
        const options = {background: true};

        // The number of indexes created here is also stored in data.nIndexes.
        assertWorkedHandleTxnErrors(coll.createIndex({text: 'text'}, options),
                                    ErrorCodes.IndexBuildAlreadyInProgress);
        assertWorkedHandleTxnErrors(coll.createIndex({geo: '2dsphere'}, options),
                                    ErrorCodes.IndexBuildAlreadyInProgress);
        assertWorkedHandleTxnErrors(coll.createIndex({integer: 1}, options),
                                    ErrorCodes.IndexBuildAlreadyInProgress);
        assertWorkedHandleTxnErrors(coll.createIndex({"$**": 1}, options),
                                    ErrorCodes.IndexBuildAlreadyInProgress);
    };

    return $config;
});
