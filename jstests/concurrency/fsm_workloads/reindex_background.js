'use strict';

/**
 * reindex_background.js
 *
 * Bulk inserts 1000 documents and builds indexes in background, then alternates between reindexing
 * and querying against the collection. Operates on a separate collection for each thread. Note
 * that because indexes are initially built in the background, reindexing is also done in the
 * background.
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/reindex.js');     // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.prefix = 'reindex_background';

    $config.states.createIndexes = function createIndexes(db, collName) {
        var coll = db[this.threadCollName];

        // The number of indexes created here is also stored in data.nIndexes
        var textResult = coll.ensureIndex({text: 'text'}, {background: true});
        assertAlways.commandWorked(textResult);

        var geoResult = coll.ensureIndex({geo: '2dsphere'}, {background: true});
        assertAlways.commandWorked(geoResult);

        var integerResult = coll.ensureIndex({integer: 1}, {background: true});
        assertAlways.commandWorked(integerResult);
    };

    return $config;
});
