/**
 * convert_to_capped_collection_index.js
 *
 * Creates a non-capped collection. Converts it to a
 * capped collection. After each iteration, truncates the
 * collection, ensuring that the storage size of the
 * collection is still a multiple of 256.
 *
 * MongoDB raises the storage size of a capped collection
 * to an integer multiple of 256.
 *
 * Make sure that we can create indexes on any collection
 * but that only the _id index remains after (re-)converting
 * to a capped collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                    // for extendWorkload
load('jstests/concurrency/fsm_workloads/convert_to_capped_collection.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.states.convertToCapped = function convertToCapped(db, collName) {
        assertWhenOwnDB.commandWorked(db[this.threadCollName].ensureIndex({i: 1, rand: 1}));
        assertWhenOwnDB.eq(2, db[this.threadCollName].getIndexes().length);
        $super.states.convertToCapped.apply(this, arguments);
    };

    return $config;
});
