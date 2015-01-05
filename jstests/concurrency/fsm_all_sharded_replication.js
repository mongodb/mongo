'use strict';

load('jstests/concurrency/fsm_libs/runner.js');

var dir = 'jstests/concurrency/fsm_workloads';

var blacklist = [
    // Disabled due to known bugs
    'agg_match.js', // SERVER-3645 .count() can be wrong on sharded collections
    'count.js', // SERVER-3645 .count() can be wrong on sharded collections
    'count_limit_skip.js', // SERVER-3645 .count() can be wrong on sharded collections
    'count_noindex.js', // SERVER-3645 .count() can be wrong on sharded collections

    // Disabled due to MongoDB restrictions and/or workload restrictions

    // These workloads sometimes trigger 'Could not lock auth data update lock'
    // errors because the AuthorizationManager currently waits for only five
    // seconds to acquire the lock for authorization documents
    'auth_create_role.js',
    'auth_create_user.js',
    'auth_drop_role.js',
    'auth_drop_user.js', // SERVER-16739 OpenSSL libcrypto crash

    // These workloads are disabled because of recent changes in capped
    // collection behavior with wiredTiger (see: SERVER-16235)
    'create_capped_collection.js',
    'create_capped_collection_maxdocs.js',

    'agg_group_external.js', // uses >100MB of data, and is flaky
    'agg_sort_external.js', // uses >100MB of data, and is flaky
    'compact.js', // compact can only be run against a standalone mongod
    'compact_simultaneous_padding_bytes.js', // compact can only be run against a mongod
    'convert_to_capped_collection.js', // convertToCapped can't be run on mongos processes
    'convert_to_capped_collection_index.js', // convertToCapped can't be run on mongos processes
    'findAndModify_remove.js', // our findAndModify queries lack shard keys
    'findAndModify_update.js', // our findAndModify queries lack shard keys
    'findAndModify_update_collscan.js', // our findAndModify queries lack shard keys
    'findAndModify_update_grow.js', // our findAndModify queries lack shard keys
    'findAndModify_upsert.js', // our findAndModify queries lack shard keys
    'findAndModify_upsert_collscan.js', // our findAndModify queries lack shard keys
    'group.js', // the group command cannot be issued against a sharded cluster
    'group_cond.js', // the group command cannot be issued against a sharded cluster
    'indexed_insert_eval.js', // eval doesn't work with sharded collections
    'indexed_insert_eval_nolock.js', // eval doesn't work with sharded collections
    'remove_single_document.js', // our .remove(query, {justOne: true}) calls lack shard keys
    'remove_single_document_eval.js', // eval doesn't work with sharded collections
    'remove_single_document_eval_nolock.js', // eval doesn't work with sharded collections
    'update_simple_eval.js', // eval doesn't work with sharded collections
    'update_simple_eval_nolock.js', // eval doesn't work with sharded collections
    'update_upsert_multi.js', // our update queries lack shard keys
].map(function(file) { return dir + '/' + file; });

// SERVER-16196 re-enable executing workloads against sharded replica sets
// runWorkloadsSerially(ls(dir).filter(function(file) {
//     return !Array.contains(blacklist, file);
// }), { sharded: true, replication: true });
