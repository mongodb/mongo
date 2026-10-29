/**
 * Top-level chunk operation error handler. Dispatches to the per-operation acceptability function
 * based on the operation being performed.
 */
import {Operation} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_errors.js";
import {isMoveChunkErrorAcceptableWithConcurrent} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/move_chunk_errors.js";
import {isMergeChunkErrorAcceptableWithConcurrent} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/merge_chunk_errors.js";
import {isSplitChunkErrorAcceptableWithConcurrent} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/split_chunk_errors.js";

export function isErrorAcceptableWithConcurrent(operation, concurrentOperations, error) {
    if (operation === Operation.MoveChunk) {
        return isMoveChunkErrorAcceptableWithConcurrent(concurrentOperations, error);
    }
    if (operation === Operation.SplitChunk) {
        return isSplitChunkErrorAcceptableWithConcurrent(concurrentOperations, error);
    }
    if (operation === Operation.MergeChunks) {
        return isMergeChunkErrorAcceptableWithConcurrent(concurrentOperations, error);
    }
    assert(false, `No error acceptability handler for operation: ${operation}`);
}
