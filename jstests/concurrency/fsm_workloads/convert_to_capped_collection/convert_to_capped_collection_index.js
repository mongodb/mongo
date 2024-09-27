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
 *
 * @tags: [
 *   # convertToCapped can't be run on sharded collections.
 *   assumes_unsharded_collection,
 *   # convertToCapped requires a global lock and any background operations on the database causes
 *   # it to fail due to not finishing quickly enough.
 *   incompatible_with_concurrency_simultaneous,
 *   requires_collstats,
 *   requires_capped
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/convert_to_capped_collection/convert_to_capped_collection.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.convertToCapped = function convertToCapped(db, collName) {
        assert.commandWorked(db[this.threadCollName].createIndex({i: 1, rand: 1}));
        assert.eq(2, db[this.threadCollName].getIndexes().length);
        $super.states.convertToCapped.apply(this, arguments);
    };

    return $config;
});
