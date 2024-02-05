/**
 * Technically this is not time series colleciton test; however, due to legacy behavior, a user
 * inserts into a collection in time series bucket namespace is required not to fail.  Please note
 * this behavior is likely going to be corrected in 6.3 or after. The presence of this test does
 * not imply such behavior is supported.
 *
 * As this tests code path relevant to time series, the requires_tiemseries flag is set to avoid
 * incompatible behavior related to multi statement transactions.
 *
 *  @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_63,
 *   # TODO SERVER-85382: re-enable in ugprade/downgrade suites
 *   cannot_run_during_upgrade_downgrade
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const isTrackingUnsplittableCollections = FeatureFlagUtil.isPresentAndEnabled(
    db.getSiblingDB('admin'), "TrackUnshardedCollectionsOnShardingCatalog");

// TODO SERVER-85382 re-enable this test with tracked collection once create collection coordinator
// support all timeseries/bucket namespace cases
if (!isTrackingUnsplittableCollections) {
    let userCollSystemBuckets = db.system.buckets.coll;
    let userColl = db.coll;

    userCollSystemBuckets.drop();
    userColl.drop();

    // inserting into a user defined system buckets collection is possible
    assert.commandWorked(userCollSystemBuckets.insert({a: 1}));

    // A user collection with the same postfix should not be considered time series collection
    assert.commandWorked(userColl.insert({a: 2}));

    let docs = userColl.find().toArray();
    assert.eq(1, docs.length);

    let docsSystemBuckets = userCollSystemBuckets.find().toArray();
    assert.eq(1, docsSystemBuckets.length);

    userCollSystemBuckets.drop();
    userColl.drop();

    // the sequence in different order should also work
    assert.commandWorked(userColl.insert({a: 2}));
    assert.commandWorked(userCollSystemBuckets.insert({a: 1}));

    docs = userColl.find().toArray();
    assert.eq(1, docs.length);

    docsSystemBuckets = userCollSystemBuckets.find().toArray();
    assert.eq(1, docsSystemBuckets.length);

    userCollSystemBuckets.drop();
    userColl.drop();
}
