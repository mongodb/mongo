/**
 * Common error-handling framework and shared types for chunk operation FSM workloads.
 */

export const Operation = {
    MoveChunk: "moveChunk",
    SplitChunk: "splitChunk",
    MergeChunks: "mergeChunks",
};

export const ConcurrentOperation = {
    MoveChunk: "moveChunk",
    RemoveShard: "removeShard",
    BroadcastWrite: "broadcastWrite",
    ShardKeyUpdate: "shardKeyUpdate",
    RefineShardKey: "refineShardKey",
    CoordinatedMultiWrite: "coordinatedMultiWrite",
    ValidationLevelChange: "validationLevelChange",
    SplitChunk: "splitChunk",
    MergeChunks: "mergeChunks",
};

export function isErrorAcceptable(error, acceptableCodes, acceptableReasons) {
    if (acceptableCodes.includes(error.code)) {
        return true;
    }
    const fullMessage = formatErrorMsg(error.message, error.extraAttr);
    return acceptableReasons.some((reason) => fullMessage.includes(reason));
}

export function isCommonChunkOpContentionError(error) {
    // Lock/slot/range-deletion contention any chunk operation can hit when another coordinator
    // holds the namespace's critical section or the per-shard migration slot, or a prior
    // migration's range deletion is still pending. Inherent to running chunk ops concurrently;
    // not a correctness problem.
    const acceptableCodes = [
        ErrorCodes.ConflictingOperationInProgress,
        ErrorCodes.LockBusy,
        ErrorCodes.LockTimeout,
        ErrorCodes.ChunkRangeCleanupPending,
    ];
    return isErrorAcceptable(error, acceptableCodes, []);
}

export function isErrorAcceptableWithSplitOrMerge(error) {
    // A concurrent split or merge rewrites chunk boundaries between the moment another chunk
    // operation snapshots a chunk and the moment that operation reaches the shard or the
    // config-server commit. The snapshot is then stale, and the in-flight operation surfaces one
    // of the following purely because of that race; none indicates a correctness problem.
    //
    //   - StaleConfig: the shard targeted by the command no longer owns the range, or split's
    //     shard-side check finds no chunk with those bounds on this shard.
    //   - IncompatibleShardingMetadata: the config-server split-commit path cannot find the chunk
    //     by min key because metadata has been rewritten.
    //   - "Could not meet precondition to split chunk": the config-server split-commit transaction
    //     finds the origin chunk mutated between the router's check and commit. BadValue is too
    //     generic to whitelist outright, so we key off the specific message.
    //   - "no chunk found with the shard key bounds ...": the split router-side check reports this
    //     via ErrmsgCommandDeprecated, which returns ok:0 without a top-level code, so we match by
    //     message.
    //
    // The mongos routing layer retries StaleConfig transparently for CRUD, but DDL commands like
    // split/merge/moveChunk surface these directly to the client.
    const acceptableCodes = [ErrorCodes.StaleConfig, ErrorCodes.IncompatibleShardingMetadata];
    const acceptableReasons = [
        "Could not meet precondition to split chunk",
        "no chunk found with the shard key bounds",
    ];
    return (
        isCommonChunkOpContentionError(error) ||
        isErrorAcceptable(error, acceptableCodes, acceptableReasons)
    );
}
