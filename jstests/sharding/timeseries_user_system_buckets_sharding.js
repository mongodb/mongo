/**
 * Technically this is not time series colleciton test; however, due to legacy behavior, a user
 * inserts into a collection in time series bucket namespace is required not to fail.  Please note
 * this behavior is likely going to be corrected in 6.3 or after. The presence of this test does
 * not imply such behavior is supported.
 *
 * As this tests code path relevant to time series, the requires_tiemseries flag is set to avoid
 * incompatible behavior related to multi statement transactions.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_63,
 *   assumes_no_implicit_collection_creation_on_get_collection
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({shards: 2});
assert.commandWorked(
    st.s0.adminCommand({enableSharding: 'test', primaryShard: st.shard1.shardName}));

// TODO SERVER-87189 Remove this helper as starting from 8.0 we always pass from the coordinator to
// create a collection.
const isTrackUnshardedCollectionsEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
const tsOptions = {
    timeField: "timestamp",
    metaField: "metadata"
};

const tsOptions2 = {
    timeField: "timestamp",
    metaField: "metadata2"
};

const kDbName = "test";
const kColl = "coll";
const kBucket = "system.buckets.coll";

var db = st.getDB(kDbName);

function createWorked(collName, tsOptions = {}) {
    if (Object.keys(tsOptions).length === 0) {
        assert.commandWorked(db.createCollection(collName));
    } else {
        assert.commandWorked(db.createCollection(collName, {timeseries: tsOptions}));
    }
    return db.getCollection(collName);
}

function createFailed(collName, tsOptions, errorCode) {
    if (Object.keys(tsOptions).length === 0) {
        assert.commandFailedWithCode(db.createCollection(collName), errorCode);
    } else {
        assert.commandFailedWithCode(db.createCollection(collName, {timeseries: tsOptions}),
                                     errorCode);
    }
}

function shardCollectionWorked(collName, tsOptions = {}) {
    let nss = kDbName + "." + collName;
    if (Object.keys(tsOptions).length === 0) {
        assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {x: 1}}));
    } else {
        assert.commandWorked(
            st.s.adminCommand({shardCollection: nss, key: {timestamp: 1}, timeseries: tsOptions}));
    }
    return db.getCollection(collName);
}

function shardCollectionFailed(collName, tsOptions, errorCode) {
    let nss = kDbName + "." + collName;
    if (Object.keys(tsOptions).length === 0) {
        assert.commandFailedWithCode(st.s.adminCommand({shardCollection: nss, key: {x: 1}}),
                                     errorCode);
    } else {
        assert.commandFailedWithCode(
            st.s.adminCommand({shardCollection: nss, key: {timestamp: 1}, timeseries: tsOptions}),
            errorCode);
    }
}

function runTest(testCase, minRequiredVersion = null) {
    if (minRequiredVersion) {
        const res =
            st.s.getDB("admin").system.version.find({_id: "featureCompatibilityVersion"}).toArray();
        if (MongoRunner.compareBinVersions(res[0].version, minRequiredVersion) < 0) {
            return;
        }
    }
    testCase();
    db.dropDatabase();
}

// Case prexisting collection: standard.
{
    jsTest.log("Case collection: standard / collection: sharded standard.");
    runTest(() => {
        createWorked(kColl);
        shardCollectionWorked(kColl);
    });

    jsTest.log("Case collection: standard / collection: sharded timeseries.");
    runTest(
        () => {
            createWorked(kColl);
            shardCollectionFailed(kColl, tsOptions, ErrorCodes.InvalidOptions);
        },
        // Before 8.1 the shardCollection used to work instead of returning error.
        // TODO BACKPORT-19383 remove the minRequiredVersion
        "8.1" /*minRequiredVersion*/);
}

// Case prexisting collection: timeseries.
{
    jsTest.log("Case collection: timeseries / collection: sharded standard.");
    runTest(() => {
        createWorked(kColl, tsOptions);
        shardCollectionFailed(kColl, {}, 5914001);
    });

    jsTest.log("Case collection: timeseries / collection: sharded timeseries.");
    runTest(() => {
        createWorked(kColl, tsOptions);
        shardCollectionWorked(kColl, tsOptions);
    });

    jsTest.log("Case collection: timeseries / collection: sharded timeseries with different opts.");
    runTest(() => {
        createWorked(kColl, tsOptions);
        shardCollectionFailed(kColl, tsOptions2, [ErrorCodes.InvalidOptions]);
    });
}

// Case prexisting collection: bucket timeseries.
{
    jsTest.log("Case collection: bucket timeseries / collection: sharded standard.");
    runTest(() => {
        createWorked(kBucket, tsOptions);
        shardCollectionFailed(kColl, {}, 5914001);
    });

    jsTest.log(
        "Case collection: bucket timeseries / collection: sharded timeseries different options.");
    runTest(() => {
        createWorked(kBucket, tsOptions);
        shardCollectionFailed(kColl, tsOptions2, [ErrorCodes.InvalidOptions]);
    });

    jsTest.log("Case collection: bucket timeseries / collection: sharded timeseries.");
    runTest(
        () => {
            createWorked(kBucket, tsOptions);
            shardCollectionWorked(kColl, tsOptions);
        },
        // TODO BACKPORT-19329 On 7.0 this test case used to cause a primary node crash.
        // TODO (SERVER-88975): Remove temporary restriction requiring FCV 8.0+.
        "8.0"  // minRequiredVersion
    );
}

// Case pre-existing collection: sharded standard
{
    jsTest.log("Case collection: sharded standard / collection: standard.");
    runTest(() => {
        shardCollectionWorked(kColl);
        createWorked(kColl);
    });

    jsTest.log("Case collection: sharded standard / collection: timeseries.");
    runTest(() => {
        shardCollectionWorked(kColl);
        createFailed(kColl, tsOptions, ErrorCodes.NamespaceExists);
    });

    jsTest.log("Case collection: sharded standard / collection: bucket timeseries.");
    runTest(() => {
        shardCollectionWorked(kColl);
        if (isTrackUnshardedCollectionsEnabled) {
            createFailed(kBucket, tsOptions, ErrorCodes.NamespaceExists);
        } else {
            // TODO SERVER-85855 creating a bucket timeseries when the main namespace already exists
            // and is not timeseries should fail
            createWorked(kBucket, tsOptions);
        }
    });

    jsTest.log("Case collection: sharded standard / collection: sharded standard.");
    runTest(() => {
        shardCollectionWorked(kColl);
        shardCollectionWorked(kColl);
    });

    jsTest.log("Case collection: sharded standard / collection: sharded timeseries.");
    runTest(() => {
        shardCollectionWorked(kColl);
        shardCollectionFailed(kColl, tsOptions, [ErrorCodes.InvalidOptions]);
    });
}

// Case pre-existing collection: sharded timeseries
{
    jsTest.log("Case collection: sharded timeseries / collection: standard.");
    runTest(() => {
        shardCollectionWorked(kColl, tsOptions);
        createFailed(kColl, {}, ErrorCodes.NamespaceExists);
    });

    jsTest.log("Case collection: sharded timeseries / collection: timeseries.");
    runTest(
        () => {
            shardCollectionWorked(kColl, tsOptions);
            createWorked(kColl, tsOptions);
        },
        // On 7.0 this test case used to wrongly fail with NamespaceExists.
        "7.1"  // minRequiredVersion
    );

    jsTest.log("Case collection: sharded timeseries / collection: timeseries with different opts.");
    runTest(() => {
        shardCollectionWorked(kColl, tsOptions);
        createFailed(kColl, tsOptions2, ErrorCodes.NamespaceExists);
    });

    jsTest.log("Case collection: sharded timeseries / collection: bucket timeseries.");
    runTest(
        () => {
            shardCollectionWorked(kColl, tsOptions);
            createWorked(kBucket, tsOptions);
        },
        // Creation of bucket namespace is not idempotent before 8.0 (SERVER-89827)
        "8.0"  // minRequiredVersion
    );

    jsTest.log("Case collection: sharded timeseries / collection: sharded standard.");
    runTest(() => {
        shardCollectionWorked(kColl, tsOptions);
        shardCollectionFailed(kColl, {}, ErrorCodes.AlreadyInitialized);
    });

    jsTest.log("Case collection: sharded timeseries / collection: sharded timeseries.");
    runTest(() => {
        shardCollectionWorked(kColl, tsOptions);
        shardCollectionWorked(kColl, tsOptions);
    });

    jsTest.log(
        "Creation of unsharded bucket collections without timeseries options is not permitted.");
    runTest(
        () => {
            createFailed(kBucket, {}, ErrorCodes.IllegalOperation);
        },
        // TODO BACKPORT-20546: Remove minRequired version once the backport is completed.
        "8.1"  // minRequiredVersion
    );

    jsTest.log(
        "Creation of sharded bucket collections without timeseries options is not permitted.");
    runTest(() => {
        shardCollectionFailed(kBucket, {}, 5731501);
    });
}

st.stop();
