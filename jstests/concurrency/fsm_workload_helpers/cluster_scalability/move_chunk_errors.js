import {
    ConcurrentOperation,
    isCommonChunkOpContentionError,
    isErrorAcceptable,
    isErrorAcceptableWithSplitOrMerge,
} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_errors.js";
const acceptabilityHandlers = new Map([
    [ConcurrentOperation.MoveChunk, isErrorAcceptableWithMoveChunk],
    [ConcurrentOperation.RemoveShard, isErrorAcceptableWithRemoveShard],
    [ConcurrentOperation.BroadcastWrite, isErrorAcceptableWithBroadcastWrite],
    [ConcurrentOperation.ShardKeyUpdate, isErrorAcceptableWithShardKeyUpdate],
    [ConcurrentOperation.RefineShardKey, isErrorAcceptableWithRefineShardKey],
    [ConcurrentOperation.CoordinatedMultiWrite, isErrorAcceptableWithCoordinatedMultiWrite],
    [ConcurrentOperation.ValidationLevelChange, isErrorAcceptableWithValidationLevelChange],
    [ConcurrentOperation.SplitChunk, isErrorAcceptableWithSplitOrMerge],
    [ConcurrentOperation.MergeChunks, isErrorAcceptableWithSplitOrMerge],
]);

function getConcurrentOperationsFromConfig() {
    const operations = [];
    if (TestData.runningWithBalancer) {
        operations.push(ConcurrentOperation.MoveChunk);
    }
    if (TestData.shardsAddedRemoved) {
        operations.push(ConcurrentOperation.RemoveShard);
    }
    return operations;
}

export function isMoveChunkErrorAcceptableWithConcurrent(operations, error) {
    const allOperations = [...getConcurrentOperationsFromConfig(), ...operations];
    let acceptable = false;
    for (const operation of allOperations) {
        const isAcceptable = acceptabilityHandlers.get(operation);
        acceptable = acceptable || isAcceptable(error);
    }
    return acceptable;
}

function isErrorAcceptableWithMoveChunk(error) {
    // When the balancer is enabled, manual chunk migrations might conflict with the operations
    // being done by the balancer.
    const acceptableCodes = [656452, 11089203];
    const acceptableReasons = [
        // A concurrent migration or an interrupt during the commit-clone phase aborts an in-flight
        // moveChunk before it commits. The command surfaces CommandFailed; the migration rolls back
        // cleanly (chunk stays on the donor, orphans cleaned up), so no document is moved or lost.
        "startCommit failed",
        // The donor migrates config.transactions entries for retryable writes; an entry older than
        // what the recipient already has uasserts TransactionTooOld. No document is lost or
        // duplicated.
        "migrate failed: TransactionTooOld",
        // A migration recipient rejects the move when its local shard catalog still holds a chunk
        // overlapping the incoming range that is reachable by point-in-time reads. This is a
        // by-design rejection; the move can succeed later, once the stale entry ages past the
        // snapshot-history window.
        "point-in-time reachable ownership history",
    ];
    return (
        isCommonChunkOpContentionError(error) ||
        isErrorAcceptable(error, acceptableCodes, acceptableReasons)
    );
}

function isErrorAcceptableWithRemoveShard(error) {
    const acceptableCodes = [ErrorCodes.ShardNotFound];
    const acceptableReasons = [
        "ShardNotFound",
        "ExceededTimeLimit",
        "InterruptedDueToReplStateChange",
        "is currently draining",
        "Location6718402",
        "startCommit failed",
    ];
    return isErrorAcceptable(error, acceptableCodes, acceptableReasons);
}

function isErrorAcceptableWithBroadcastWrite(error) {
    // Because updates don't have a shard filter stage, a migration may fail if a
    // broadcast update is operating on orphans from a previous migration in the range being
    // migrated back in. The particular error code is replaced with a more generic one, so this is
    // identified by the failed migration's error message.
    const acceptableCodes = [];
    const acceptableReasons = ["CommandFailed", "Documents in target range may still be in use"];
    return isErrorAcceptable(error, acceptableCodes, acceptableReasons);
}

function isErrorAcceptableWithShardKeyUpdate(error) {
    // These errors can occur when the test updates the shard key value of a document
    // whose chunk has been moved to another shard. Receiving a chunk only waits for
    // documents with shard key values in that range to have been cleaned up by the
    // range deleter. So, if the range deleter has not yet cleaned up that document when
    // the chunk is moved back to the original shard, the moveChunk may fail as a result
    // of a duplicate key error on the recipient.
    const acceptableCodes = [];
    const acceptableReasons = ["Location51008", "Location6718402", "Location16977"];
    return isErrorAcceptable(error, acceptableCodes, acceptableReasons);
}

function isErrorAcceptableWithRefineShardKey(error) {
    const acceptableCodes = [
        ErrorCodes.ShardKeyNotFound,
        ErrorCodes.LockTimeout,
        ErrorCodes.Interrupted,
        ErrorCodes.OrphanedRangeCleanUpFailed,
    ];
    const acceptableReasons = [
        // This error will occur as a result of trying to move a chunk with a
        // pre-refine collection epoch.
        "collection may have been dropped",
        // This error will occur if a moveChunk command has been sent with chunk
        // boundaries that represent the pre-refine chunks, but the collection has
        // already been changed to possess the post-refine chunk boundaries.
        "not valid for shard key pattern",
    ];
    return isErrorAcceptable(error, acceptableCodes, acceptableReasons);
}

function isErrorAcceptableWithCoordinatedMultiWrite(error) {
    // Coordinated multi writes will interrupt ongoing chunk migrations.
    const acceptableCodes = [ErrorCodes.Interrupted];
    const acceptableReasons = [];
    return isErrorAcceptable(error, acceptableCodes, acceptableReasons);
}

function isErrorAcceptableWithValidationLevelChange(error) {
    const acceptableCodes = [
        // collMod acquires an X lock on the collection, which can interrupt a migration mid-flight.
        ErrorCodes.Interrupted,
    ];
    const acceptableReasons = [
        // Orphan cleanup from a prior migration can block the next inbound migration from
        // receiving a range that overlaps with documents still being deleted.
        "orphans cleanup",
        // The document scan inside upgradeToConstraint holds collection resources long enough for
        // a concurrent migration's waitForClean() to observe metadata in a transitional state.
        "metadata reset",
        // The collMod holds the collection critical section while scanning, preventing a concurrent
        // migration from entering its commit phase.
        "startCommit failed",
    ];
    return isErrorAcceptable(error, acceptableCodes, acceptableReasons);
}
