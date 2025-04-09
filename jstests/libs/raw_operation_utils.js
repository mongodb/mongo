/**
 * Utility library to simplify using raw operations.
 *
 * Raw operations are supported starting with binary version 9.0.
 *
 * To use raw operations in a test without excluding it
 * from multiversion suites, use the following pattern:
 * ```javascript
 *   import {
 *     getTimeseriesCollForRawOps,
 *	   kRawOperationSpec,
 *	 } from "jstests/libs/raw_operation_utils.js";
 *
 *   const bucketsColl = getTimeseriesCollForRawOps(db, coll);
 *   let rawBucketsDocs = bucketsColl.find().rawData().toArray();
 *   let rawBucketsDocs = bucketsColl.aggregate(
 *                                [{$match: {}}],
 (                                kRawOperationSpec).toArray();
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

export function isBinaryCompatibleFlagEnabledAndStable(db, flagName) {
    const flagDoc = FeatureFlagUtil.getFeatureFlagDoc(db, flagName);
    if (!flagDoc ||
        FeatureFlagUtil.getFeatureFlagDocStatus(db, flagDoc) !==
            FeatureFlagUtil.FlagStatus.kEnabled) {
        return false;
    }

    assert.hasFields(flagDoc, ['shouldBeFCVGated', 'version']);
    assert(!flagDoc.shouldBeFCVGated);

    const flagEnabledInLastContinuous =
        MongoRunner.compareBinVersions(flagDoc.version, lastContinuousFCV) <= 0;

    const flagEnabledInLastLTS = MongoRunner.compareBinVersions(flagDoc.version, lastLTSFCV) <= 0;

    const isMultiversion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) ||
        Boolean(TestData.multiversionBinVersion);

    if ((!flagEnabledInLastLTS || !flagEnabledInLastContinuous) && isMultiversion) {
        return false;
    }

    return true;
}

// TODO SERVER-103187 remove this constant once 9.0 becomes last LTS
export const kIsRawOperationSupported =
    isBinaryCompatibleFlagEnabledAndStable(db, 'RawDataCrudOperations');
export const kRawOperationFieldName = 'rawData';
export const kRawOperationSpec = kIsRawOperationSupported ? {[kRawOperationFieldName]: true} : {};

/**
 * Given a timeseries collection return its corresponding collection that expose raw data.
 *
 * When rawData is supported this will return the original collection, as the user can simply invoke
 * rawData operations on the main collection.
 *
 * When rawData is not supported this function returns the underlying system.buckets collection.
 */
export function getTimeseriesCollForRawOps(db, coll) {
    jsTest.log(`Receinved parameter '${tojson(coll)}' (${typeof coll})`);
    if (kIsRawOperationSupported) {
        return coll;
    }
    return getTimeseriesBucketsColl(coll);
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
        if (kIsRawOperationSupported) {
            // Call the original rawData function with the (potentially) modified arguments
            return originalRawData.apply(this, value);
        } else {
            assert(
                value === undefined || value === true,
                "Detected illegal usage of rawData function. rawData(false) can't be used on a cluster with binary versions that do not support 'rawData' parameter.");
            assert(
                this._collection.getName().startsWith('system.buckets.'),
                `Detected illegal usage of rawData function. The query target '${this._collection.getName()}' that is not a system.buckets collection and the cluster contains binaries that do not support 'rawData' parameter.`);
            return this;
        }
    };
} else {
    throw new Error(
        "Failed to override DBQuery.rawData method becuase DBQuery class or its rawData method were not found");
}
})();
