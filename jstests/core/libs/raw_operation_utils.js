/**
 * Utility library based on jstests/libs/raw_operation_utils.js, automatically providing the db for
 * convenience in core tests.
 */
import * as raw from "jstests/libs/raw_operation_utils.js";

// TODO (SERVER-103187): Remove this constant once v9.0 becomes last-LTS.
export const kIsRawOperationSupported = raw.isRawOperationSupported(db);

export const kRawOperationFieldName = raw.kRawOperationFieldName;
export const kRawOperationSpec = raw.getRawOperationSpec(db);

export function getTimeseriesCollForRawOps(coll) {
    return raw.getTimeseriesCollForRawOps(db, coll);
}

export function getTimeseriesExecutionNamespace(coll) {
    return raw.getTimeseriesExecutionNamespace(db, coll);
}

export function createRawTimeseriesIndex(coll, spec, options, commitQuorum, cmdArgs) {
    return getTimeseriesCollForRawOps(coll).createIndex(
        spec, options, commitQuorum, {...kRawOperationSpec, ...cmdArgs});
}
