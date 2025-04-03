/*
 * TODO SERVER-101609 remove this library once 9.0 becomes lastLTS
 * By then only viewless timeseries will exists so we won't need these functionalities
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export function areViewlessTimeseriesEnabled(db) {
    return FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections");
}
/**
 * Given a collection return its corresponding buckets collection.
 *
 * - If the input 'coll' is a DBCollection object (representing the time-series collection),
 * this function returns a DBCollection object for the corresponding system.buckets.*
 * collection residing in the same database.
 * - If the input 'coll' is a string (the name of the time-series collection),
 * this function returns the corresponding system.buckets.* collection name as a string.
 *
 * TODO SERVER-101609 remove this function once 9.0 becomes lastLTS.
 */
export function getTimeseriesBucketsColl(coll) {
    const kBucketsPrefix = "system.buckets.";

    if (typeof coll === 'string') {
        // It's a collection name string
        if (coll.trim() === "") {
            throw new Error("Input collection name string cannot be empty.");
        }
        if (coll.startsWith(kBucketsPrefix)) {
            return coll;
        }
        return kBucketsPrefix + coll;
    }
    if (coll instanceof DBCollection) {
        const bucketsName = getTimeseriesBucketsColl(coll.getName());
        return coll.getDB().getCollection(bucketsName);
    }

    // Handle invalid input types
    throw new Error(
        `Invalid parameter. 'coll' must be a collection (DBCollection) or the collection name (string). Receinved parameter '${
            tojson(coll)}' (${typeof coll})`);
}
