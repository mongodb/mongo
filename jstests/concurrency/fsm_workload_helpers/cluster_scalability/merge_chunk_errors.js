/**
 * Error acceptability functions for mergeChunks operations running concurrently with other chunk
 * ops.
 */
import {
    isErrorAcceptable,
    isErrorAcceptableWithSplitOrMerge,
} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_errors.js";

function isErrorAcceptableWithMergeChunks(error) {
    const acceptableCodes = [];
    const acceptableReasons = [
        // A concurrent migration moved one of the snapshotted adjacent chunks off the shard before
        // the merge committed, so it no longer owns a gapless run over the range. Metadata stays
        // consistent; only this thread's stale view made the attempt miss.
        "does not contain a sequence of chunks that exactly fills the range",
    ];
    return isErrorAcceptable(error, acceptableCodes, acceptableReasons);
}

export function isMergeChunkErrorAcceptableWithConcurrent(concurrentOperations, error) {
    return isErrorAcceptableWithSplitOrMerge(error) || isErrorAcceptableWithMergeChunks(error);
}
