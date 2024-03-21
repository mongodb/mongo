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
var st = new ShardingTest({shards: 2});
assert.commandWorked(
    st.s0.adminCommand({enableSharding: 'test', primaryShard: st.shard1.shardName}));

const tsOptions = {
    timeField: "timestamp",
    metaField: "metadata"
};

const tsOptions2 = {
    timeField: "timestamp",
    metaField: "metadata2"
};

const kDbName = "test"
const kColl = "coll"
const kBucket = "system.buckets.coll"

var db = st.getDB(kDbName);

function createWorked(nss, tsOptions = {}) {
    if (Object.keys(tsOptions).length === 0) {
        assert.commandWorked(db.createCollection(nss));
    } else {
        assert.commandWorked(db.createCollection(nss, {timeseries: tsOptions}));
    }
    return db.getCollection(nss);
}

function createFailed(nss, tsOptions, errorCode) {
    if (Object.keys(tsOptions).length === 0) {
        assert.commandFailedWithCode(db.createCollection(nss), errorCode);
    } else {
        assert.commandFailedWithCode(db.createCollection(nss, {timeseries: tsOptions}), errorCode);
    }
}

function shardCollectionWorked(coll, tsOptions = {}) {
    let nss = kDbName + "." + coll
    if (Object.keys(tsOptions).length === 0) {
        assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {x: 1}}));
    }
    else {
        assert.commandWorked(
            st.s.adminCommand({shardCollection: nss, key: {timestamp: 1}, timeseries: tsOptions}));
    }
    return db.getCollection(nss);
}

function shardCollectionFailed(coll, tsOptions, errorCode) {
    let nss = kDbName + "." + coll
    if (Object.keys(tsOptions).length === 0) {
        assert.commandFailedWithCode(st.s.adminCommand({shardCollection: nss, key: {x: 1}}),
                                     errorCode);
    }
    else {
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
    testCase()
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
            shardCollectionWorked(kColl, tsOptions);
        },
        // On 7.0 this test case used to wrongly fail with NamespaceExists.
        "7.1"  // minRequiredVersion
    );
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
        shardCollectionFailed(kColl, tsOptions2, 5731500);
    });
}

// Case prexisting collection: bucket standard.
{
    jsTest.log("Case collection: bucket standard / collection: sharded standard.");
    runTest(() => {
        createWorked(kBucket);
        shardCollectionFailed(kColl, {}, 6159000);
    });

    jsTest.log("Case collection: bucket standard / collection: sharded timeseries.");
    runTest(() => {
        createWorked(kBucket);
        shardCollectionFailed(kColl, tsOptions, 6159000);
    });
}

// Case prexisting collection: bucket timeseries.
{
    jsTest.log("Case collection: bucket timeseries / collection: sharded standard.");
    runTest(() => {
        createWorked(kBucket, tsOptions);
        shardCollectionFailed(kColl, {}, 5914001);
    });

    jsTest.log("Case collection: bucket timeseries / collection: sharded timeseries.");
    runTest(
        () => {
            createWorked(kBucket, tsOptions);
            let coll = shardCollectionWorked(kColl, tsOptions);

            // TODO SERVER-85855 remove all the code below once creating a timeseries bucket
            // collection will create the related view. Before the fix, the shardCollection returns
            // a pointer to a "test.test.coll" which doesn't exist. An invalid insert afterwords
            // will work and create the collection as unsharded. After the fix, we expect this case
            // to behave same as sharding an existing timeseries collection.
            assert.commandWorked(coll.insert({x: 1}));
            let listCollections = db.runCommand({listCollections: 1}).cursor.firstBatch;
            listCollections.sort((a, b) => a.name < b.name);  // collection name alphabetic order

            assert.eq(2, listCollections.length);
            assert.eq(kBucket, listCollections[1].name);
            // note the collection name is "test.coll and not just coll"
            assert.eq("test.coll", listCollections[0].name);
        },
        // TODO SERVER-85855 On 7.0 this test case used to cause a primary node crash.
        "7.1"  // minRequiredVersion
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

    jsTest.log("Case collection: sharded standard / collection: bucket.");
    runTest(() => {
        let coll = shardCollectionWorked(kColl);
        let bucket = createWorked(kBucket);

        bucket.insert({x: 1});
        var docsSystemBuckets = bucket.find().toArray();
        assert.eq(1, docsSystemBuckets.length);

        coll.insert({x: 1})
        var docs = coll.find().toArray();
        assert.eq(1, docs.length);
    });

    jsTest.log("Case collection: sharded standard / collection: bucket timeseries.");
    runTest(() => {
        let coll = shardCollectionWorked(kColl);
        createWorked(kBucket, tsOptions);

        coll.insert({x: 1})
        var docs = coll.find().toArray();
        assert.eq(1, docs.length);
    });

    jsTest.log("Case collection: sharded standard / collection: sharded standard.");
    runTest(() => {
        shardCollectionWorked(kColl);
        shardCollectionWorked(kColl);
    });

    jsTest.log("Case collection: sharded standard / collection: sharded timeseries.");
    runTest(() => {
        shardCollectionWorked(kColl);
        shardCollectionFailed(kColl, tsOptions, ErrorCodes.AlreadyInitialized);
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
    jsTest.log("Case collection: sharded timeseries / collection: bucket.");
    runTest(() => {
        shardCollectionWorked(kColl, tsOptions);
        createFailed(kBucket, {}, ErrorCodes.NamespaceExists);
    });

    jsTest.log("Case collection: sharded timeseries / collection: bucket timeseries.");
    runTest(() => {
        shardCollectionWorked(kColl, tsOptions);
        createFailed(kBucket, tsOptions, ErrorCodes.NamespaceExists);
    });

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
}

st.stop();
