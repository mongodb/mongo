'use strict';

/**
 * Runs updateOne, deleteOne, and findAndModify without shard key against a sharded cluster while
 * there are concurrent chunk migrations.
 *
 * @tags: [
 *  featureFlagUpdateOneWithoutShardKey,
 *  requires_fcv_70,
 *  requires_sharding,
 *  uses_transactions,
 * ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/write_without_shard_key_base.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.startState = "init";
    $config.transitions = {
        init: {moveChunk: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2},
        updateOne: {moveChunk: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2},
        deleteOne: {moveChunk: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2},
        findAndModify: {moveChunk: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2},
        moveChunk: {moveChunk: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2}
    };

    // Because updates don't have a shard filter stage, a migration may fail if a
    // broadcast update is operating on orphans from a previous migration in the range being
    // migrated back in. The particular error code is replaced with a more generic one, so this is
    // identified by the failed migration's error message.
    $config.data.isMoveChunkErrorAcceptable = (err) => {
        return err.message &&
            (err.message.includes("CommandFailed") ||
             err.message.includes("Documents in target range may still be in use") ||
             // This error can occur when the test updates the shard key value of a document whose
             // chunk has been moved to another shard. Receiving a chunk only waits for documents
             // with shard key values in that range to have been cleaned up by the range deleter.
             // So, if the range deleter has not yet cleaned up that document when the chunk is
             // moved back to the original shard, the moveChunk may fail as a result of a duplicate
             // key error on the recipient.
             err.message.includes("Location51008") || err.message.includes("Location6718402"));
    };

    return $config;
});
