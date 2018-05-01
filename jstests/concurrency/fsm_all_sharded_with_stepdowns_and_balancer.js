'use strict';

load('jstests/concurrency/fsm_libs/runner.js');

var dir = 'jstests/concurrency/fsm_workloads';

var blacklist = [
    // Disabled due to known bugs
    'distinct.js',             // SERVER-13116 distinct isn't sharding aware
    'distinct_noindex.js',     // SERVER-13116 distinct isn't sharding aware
    'distinct_projection.js',  // SERVER-13116 distinct isn't sharding aware
    'create_database.js',      // SERVER-17397 Drops of sharded namespaces may not fully succeed
    'drop_database.js',        // SERVER-17397 Drops of sharded namespaces may not fully succeed
    'remove_where.js',  // SERVER-14669 Multi-removes that use $where miscount removed documents

    // Disabled due to SERVER-33753, '.count() without a predicate can be wrong on sharded
    // collections'. This bug is problematic for these workloads because they assert on count()
    // values:
    'agg_match.js',

    // $lookup and $graphLookup are not supported on sharded collections.
    'agg_graph_lookup.js',
    'view_catalog_cycle_lookup.js',

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
    'map_reduce_replace_remove.js',

    // Disabled due to SERVER-13364, 'The geoNear command doesn't handle shard versioning, so a
    // concurrent chunk migration may cause duplicate or missing results'
    'yield_geo_near_dedup.js',

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
    'findAndModify_mixed_queue_unindexed.js',   // findAndModify requires a shard key
    'findAndModify_remove_queue.js',            // remove cannot be {} for findAndModify
    'findAndModify_remove_queue_unindexed.js',  // findAndModify requires a shard key
    'findAndModify_update_collscan.js',         // findAndModify requires a shard key
    'findAndModify_update_grow.js',             // can cause OOM kills on test hosts
    'findAndModify_update_queue.js',            // findAndModify requires a shard key
    'findAndModify_update_queue_unindexed.js',  // findAndModify requires a shard key
    'group.js',                // the group command cannot be issued against a sharded cluster
    'group_cond.js',           // the group command cannot be issued against a sharded cluster
    'group_killop.js',         // the group command cannot be issued against a sharded cluster
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

    // ChunkHelper directly talks to the config servers and doesn't support retries for network
    // errors
    'sharded_base_partitioned.js',
    'sharded_mergeChunks_partitioned.js',
    'sharded_moveChunk_drop_shard_key_index.js',
    'sharded_moveChunk_partitioned.js',
    'sharded_splitChunk_partitioned.js',

    // These workloads frequently time out waiting for the distributed lock to drop a sharded
    // collection.
    'kill_aggregation.js',
    'kill_rooted_or.js',
    'view_catalog_cycle_with_drop.js',
    'view_catalog.js',

    // Use getmores.
    'agg_base.js',
    'create_index_background.js',
    'globally_managed_cursors.js',
    'indexed_insert_ordered_bulk.js',
    'indexed_insert_text.js',
    'indexed_insert_unordered_bulk.js',
    'indexed_insert_upsert.js',
    'indexed_insert_where.js',
    'list_indexes.js',
    'reindex.js',
    'reindex_background.js',
    'remove_multiple_documents.js',
    'remove_where.js',
    'touch_base.js',
    'touch_data.js',
    'touch_index.js',
    'touch_no_data_no_index.js',
    'update_where.js',
    'yield.js',
    'yield_fetch.js',
    'yield_rooted_or.js',
    'yield_sort.js',
    'yield_sort_merge.js',
    'yield_text.js',
    'kill_multicollection_aggregation.js',
    'invalidated_cursors.js',

    // Use non retryable writes.
    'remove_and_bulk_insert.js',
    'update_and_bulk_insert.js',
    'update_check_index.js',
    'update_multifield_multiupdate.js',
    'update_multifield_multiupdate_noindex.js',
    'update_ordered_bulk_inc.js',
    'yield_geo_near.js',
    'yield_geo_near_dedup.js',
    'yield_id_hack.js',

    // Use non retryable commands.
    'agg_out.js',
    'agg_sort.js',
    'collmod.js',
    'collmod_separate_collections.js',
    'view_catalog.js',

    // The auto_retry_on_network_error.js override needs to overwrite the response from drop on
    // NamespaceNotFound, and since this workload only creates and drops collections there isn't
    // much value in running it.
    'drop_collection.js',
].map(function(file) {
    return dir + '/' + file;
});

runWorkloadsSerially(ls(dir).filter(function(file) {
    return !Array.contains(blacklist, file);
}),
                     {
                       sharded: {
                           enabled: true,
                           enableBalancer: true,
                           stepdownOptions: {configStepdown: true, shardStepdown: true}
                       },
                       replication: {enabled: true}
                     },
                     {sessionOptions: {retryWrites: true}});
