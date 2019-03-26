'use strict';

/**
 * reindex_background.js
 *
 * Bulk inserts 1000 documents and builds indexes in background, then alternates between reindexing
 * and querying against the collection. Operates on a separate collection for each thread. Note
 * that because indexes are initially built in the background, reindexing is also done in the
 * background.
 *
 * @tags: [creates_background_indexes]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/reindex.js');     // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.prefix = 'reindex_background';

    $config.states.createIndexes = function createIndexes(db, collName) {
        const coll = db[this.threadCollName];
        const options = {background: true};

        // The number of indexes created here is also stored in data.nIndexes.
        assertAlways.commandWorked(coll.createIndex({text: 'text'}, options));
        assertAlways.commandWorked(coll.createIndex({geo: '2dsphere'}, options));
        assertAlways.commandWorked(coll.createIndex({integer: 1}, options));
        assertAlways.commandWorked(coll.createIndex({"$**": 1}, options));
    };

    return $config;
});
