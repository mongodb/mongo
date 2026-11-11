import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const kBatchBoundaryFailPoint = "pauseBatchApplicationBeforeCompletion";
const kPipelinedWorkerFailPoint = "pausePipelinedApplierWorkerBeforeNextBatch";

/**
 * Achieves the effect of pausing oplog application such that:
 * 1. All worker threads have finished working on a batch and are waiting, but
 * 2. The last applied timestamp and other end-of-batch updates haven't been published.
 *
 * Tests can call this function and have both OplogApplierImpl and POA both pause.
 *
 * It's recommended that tests call this on an idle applier. Calling on an applier that's
 * at work may result in pausing the applier too early (i.e. when a previous batch is still
 * being processed) or too late (and missing the batch that we want to hang on). For that
 * reason it's recommended that no writes are ongoing and replication has been waited on
 * before this function is called.
 */
export function configurePauseBatchApplicationBeforeCompletion(conn) {
    // Accept either a connection or a db; FeatureFlagUtil requires a db.
    const db = typeof conn.getDB === "function" ? conn.getDB("admin") : conn;
    let workerFp;
    if (FeatureFlagUtil.isPresentAndEnabled(db, "PipelinedOplogApplier")) {
        workerFp = configureFailPoint(conn, kPipelinedWorkerFailPoint);
    }
    const fp = configureFailPoint(conn, kBatchBoundaryFailPoint);
    return {
        // Returns once the fail point has been entered with the entire batch applied, on either
        // applier. Only the batch-boundary fail point is waited on: the worker fail point is
        // entered per worker slice and does not imply a batch boundary.
        wait: function (opts) {
            return fp.wait(opts);
        },
        // Disable the batch-boundary fail point first so publication resumes, then release the
        // workers.
        off: function () {
            fp.off();
            if (workerFp !== undefined) {
                workerFp.off();
            }
        },
    };
}
