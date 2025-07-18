/**
 * Utility library to simplify using raw operations.
 *
 * Raw operations are supported starting with binary version 8.2.
 *
 * To use raw operations in a test without excluding it
 * from multiversion suites, use the following pattern:
 * ```javascript
 *   import {
 *     getTimeseriesCollForRawOps,
 *	   getRawOperationSpec,
 *	 } from "jstests/libs/raw_operation_utils.js";
 *
 *   const bucketsColl = getTimeseriesCollForRawOps(db, coll);
 *   let rawBucketsDocs = bucketsColl.find().rawData().toArray();
 *   let rawBucketsDocs = bucketsColl.aggregate([{$match: {}}], getRawOperationSpec(db)).toArray();
 * ```
 * This approach ensures the code works correctly even in versions that
 * do not support raw operations. Specifically:
 * - In versions where raw operations are supported:
 *    - `getTimeseriesCollForRawOps` acts as a no-op and simply returns the original `coll`.
 *    - `rawData()` and `kRawOperationSpec` effectively add the `rawData` parameter
 *       to the operation.
 *  - On the other hand, in versions where raw operations are not supported:
 *    - `getTimeseriesCollForRawOps` returns the underlying `system.buckets` collection.
 *    - `rawData()` and `kRawOperationSpec` act as no-ops and do not attach any
 *      parameter to the operation.
 */
import {getTimeseriesBucketsColl} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// TODO SERVER-103187 remove this constant once 9.0 becomes last LTS
export const kRawOperationFieldName = 'rawData';

// TODO (SERVER-103187): Remove these functions once v9.0 becomes last-LTS.
export function isRawOperationSupported(db) {
    // getParameter can't be used inside transactions, so issue the command directly on the
    // connection, rather than using the session potentially linked to the DB object.
    const conn = db.getMongo();

    // In multiversion suites, the flag is stable only if *all* binaries in the cluster have the
    // binary-compatible flag enabled. We detect this through an FCV-gated feature flag, which works
    // because the FCV can only be upgraded once all binaries in the cluster are upgraded.
    const isMultiversion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) ||
        Boolean(TestData.multiversionBinVersion);
    if (isMultiversion) {
        const flagDoc =
            FeatureFlagUtil.getFeatureFlagDoc(conn, 'AllBinariesSupportRawDataOperations');
        if (!flagDoc ||
            FeatureFlagUtil.getFeatureFlagDocStatus(conn, flagDoc) !==
                FeatureFlagUtil.FlagStatus.kEnabled) {
            return false;
        }

        assert.hasFields(flagDoc, ['fcv_gated']);
        assert(flagDoc.fcv_gated);

        return true;
    }

    // For non-multiversion suites, check the status of the binary-compatible flag directly
    // Note that we may return true here even if 'AllBinariesSupportRawDataOperations' is disabled
    // (e.g. in FCV upgrade/downgrade suites, where all binaries are always on the latest version)
    const flagDoc = FeatureFlagUtil.getFeatureFlagDoc(conn, 'RawDataCrudOperations');
    if (!flagDoc ||
        FeatureFlagUtil.getFeatureFlagDocStatus(conn, flagDoc) !==
            FeatureFlagUtil.FlagStatus.kEnabled) {
        return false;
    }

    assert.hasFields(flagDoc, ['fcv_gated']);
    assert(!flagDoc.fcv_gated);

    return true;
}

export function getRawOperationSpec(db) {
    return isRawOperationSupported(db) ? {[kRawOperationFieldName]: true} : {};
}

/**
 * Given a timeseries collection, returns its corresponding collection that exposes raw data.
 *
 * When rawData is supported, returns the original collection as the user can simply invoke
 * rawData operations on the main collection.
 *
 * When rawData is not supported this function returns the underlying system.buckets collection.
 */
export function getTimeseriesCollForRawOps(db, coll) {
    return isRawOperationSupported(db) ? coll : getTimeseriesBucketsColl(coll);
}

/**
 * Helper function to create an index directly on the raw timeseries buckets.
 */
export function createRawTimeseriesIndex(coll, spec, options, commitQuorum, cmdArgs) {
    return getTimeseriesCollForRawOps(coll.getDB(), coll).createIndex(spec, options, commitQuorum, {
        ...getRawOperationSpec(coll.getDB()),
        ...cmdArgs
    });
}

/**
 * Override the rawData function on the DBQuery object so that it becomes a no-op on versions where
 * rawData parameter is not supported (< 9.0)
 *
 * TODO SERVER-103187 remove this override once 9.0 becomes last LTS
 */
(function() {
if (typeof DBQuery !== 'undefined' && typeof DBQuery.prototype.rawData === 'function') {
    const originalRawData = DBQuery.prototype.rawData;

    // Override the rawData function
    DBQuery.prototype.rawData = function(value) {
        if (isRawOperationSupported(this._db)) {
            // Call the original rawData function with the (potentially) modified arguments
            return originalRawData.apply(this, value);
        } else {
            assert(
                value === undefined || value === true,
                "Detected illegal usage of rawData function. rawData(false) can't be used on a cluster with binary versions that do not support 'rawData' parameter.");
            if (!this._collection.getName().startsWith('system.buckets.')) {  // Fast path
                const collMetadata = this._collection.getMetadata();
                assert(!collMetadata || collMetadata.type === 'collection',
                    `Detected illegal usage of rawData function. The query target '${this._collection.getName()}' that is not a regular collection and the cluster contains binaries that do not support 'rawData' parameter.`);
            }
            return this;
        }
    };
} else {
    throw new Error(
        "Failed to override DBQuery.rawData method becuase DBQuery class or its rawData method were not found");
}
})();
