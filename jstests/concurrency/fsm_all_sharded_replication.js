'use strict';

load('jstests/concurrency/fsm_libs/runner.js');

var dir = 'jstests/concurrency/fsm_workloads';

var blacklist = [
    // Disabled due to known bugs
    'distinct.js',             // SERVER-13116 distinct isn't sharding aware
    'distinct_noindex.js',     // SERVER-13116 distinct isn't sharding aware
    'distinct_projection.js',  // SERVER-13116 distinct isn't sharding aware
    'drop_database.js',        // SERVER-17397 Drops of sharded namespaces may not fully succeed

    // Disabled due to SERVER-3645, '.count() can be wrong on sharded collections'.
    // This bug is problematic for these workloads because they assert on count() values:
    'agg_match.js',
    'count.js',
    'count_limit_skip.js',
    'count_noindex.js',

    // $graphLookup does not support sharded clusters.
    'agg_graph_lookup.js',

    // Disabled due to SERVER-20057, 'Concurrent, sharded mapReduces can fail when temporary
    // namespaces collide across mongos processes'
    'map_reduce_drop.js',
    'map_reduce_inline.js',
    'map_reduce_merge.js',
    'map_reduce_merge_nonatomic.js',
    'map_reduce_reduce.js',
    'map_reduce_reduce_nonatomic.js',
    'map_reduce_replace.js',
    'map_reduce_replace_nonexistent.js',

    // Disabled due to MongoDB restrictions and/or workload restrictions

    // These workloads sometimes trigger 'Could not lock auth data update lock'
    // errors because the AuthorizationManager currently waits for only five
    // seconds to acquire the lock for authorization documents
    'auth_create_role.js',
    'auth_create_user.js',
    'auth_drop_role.js',
    'auth_drop_user.js',

    'agg_group_external.js',                  // uses >100MB of data, which can overwhelm test hosts
    'agg_sort_external.js',                   // uses >100MB of data, which can overwhelm test hosts
    'compact.js',                             // compact can only be run against a standalone mongod
    'compact_simultaneous_padding_bytes.js',  // compact can only be run against a mongod
    'convert_to_capped_collection.js',        // convertToCapped can't be run on mongos processes
    'convert_to_capped_collection_index.js',  // convertToCapped can't be run on mongos processes
    'findAndModify_mixed_queue.js',           // findAndModify requires a shard key
    'findAndModify_mixed_queue_unindexed.js',   // findAndModify requires a shard key
    'findAndModify_remove_queue.js',            // remove cannot be {} for findAndModify
    'findAndModify_remove_queue_unindexed.js',  // findAndModify requires a shard key
    'findAndModify_update_collscan.js',         // findAndModify requires a shard key
    'findAndModify_update_queue.js',            // findAndModify requires a shard key
    'findAndModify_update_queue_unindexed.js',  // findAndModify requires a shard key
    'group.js',                // the group command cannot be issued against a sharded cluster
    'group_cond.js',           // the group command cannot be issued against a sharded cluster
    'indexed_insert_eval.js',  // eval doesn't work with sharded collections
    'indexed_insert_eval_nolock.js',  // eval doesn't work with sharded collections
    'plan_cache_drop_database.js',  // cannot ensureIndex after dropDatabase without sharding first
    'remove_single_document.js',    // our .remove(query, {justOne: true}) calls lack shard keys
    'remove_single_document_eval.js',         // eval doesn't work with sharded collections
    'remove_single_document_eval_nolock.js',  // eval doesn't work with sharded collections

    // The rename_* workloads are disabled since renameCollection doesn't work with sharded
    // collections
    'rename_capped_collection_chain.js',
    'rename_capped_collection_dbname_chain.js',
    'rename_capped_collection_dbname_droptarget.js',
    'rename_capped_collection_droptarget.js',
    'rename_collection_chain.js',
    'rename_collection_dbname_chain.js',
    'rename_collection_dbname_droptarget.js',
    'rename_collection_droptarget.js',

    'update_simple_eval.js',           // eval doesn't work with sharded collections
    'update_simple_eval_nolock.js',    // eval doesn't work with sharded collections
    'update_upsert_multi.js',          // our update queries lack shard keys
    'update_upsert_multi_noindex.js',  // our update queries lack shard keys
    'upsert_where.js',      // cannot use upsert command with $where with sharded collections
    'yield_and_hashed.js',  // stagedebug can only be run against a standalone mongod
    'yield_and_sorted.js',  // stagedebug can only be run against a standalone mongod
].map(function(file) {
    return dir + '/' + file;
});

runWorkloadsSerially(ls(dir).filter(function(file) {
    return !Array.contains(blacklist, file);
}),
                     {sharded: true, replication: true});
