/**
 * Error acceptability functions for splitChunk operations running concurrently with other chunk ops.
 */
import {
    isErrorAcceptable,
    isErrorAcceptableWithSplitOrMerge,
} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_errors.js";

function isErrorAcceptableWithSplitChunk(error) {
    const acceptableCodes = [
        // Concurrent reshaping shrank the chunk below the two distinct shard-key values a split
        // needs.
        ErrorCodes.CannotSplit,
    ];
    const acceptableReasons = [
        // `split` finds the chunk effectively empty on one side of a MinKey/MaxKey bound; the
        // numeric pre-check cannot catch this because the bound is not a number.
        "desired range is possibly empty",
        // Splitting a chunk whose range holds no documents fails with this message when the
        // shard that owns it also has no data for the collection locally.
        "cannot use split with find or bounds option on an empty collection",
    ];
    return isErrorAcceptable(error, acceptableCodes, acceptableReasons);
}

export function isSplitChunkErrorAcceptableWithConcurrent(concurrentOperations, error) {
    return isErrorAcceptableWithSplitOrMerge(error) || isErrorAcceptableWithSplitChunk(error);
}
