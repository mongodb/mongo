/**
 * Test several conditions when dropping timeseries collections.
 *
 * @tags: [
 *   requires_timeseries,
 *   # If the timeseries collection is sharded, the behavior is different (in particular, you can't
 *   # drop a sharded timeseries collection by the buckets namespace).
 *   assumes_unsharded_collection,
 *   assumes_no_implicit_collection_creation_after_drop,
 *
 *   # drop collection is NOT retryable under the conditions of this test. Consider the scenario:
 *   #  - We have a situation like case 9 (normal collection, buckets exists, drop by the main NS).
 *   #  - Drop command of the main NS is started.
 *   #  - The collection is dropped.
 *   #  - The drop command is interrupted by a step down, drop returns with error "interrupted".
 *   #  - The command is retryed, but now the main collection doesn't exist, so the buckets
 *   #    collection is dropped instead.
 *   requires_non_retryable_commands,
 * ]
 */

const timeseriesOptions = {
    timeseries: {timeField: "t"}
};

const dummyCollection = "dummyCollection";

let _collCounter = 1;
function getNewColl(db) {
    const collNamePrefix = jsTestName() + '_coll_';
    const coll = db[collNamePrefix + _collCounter++];
    return coll;
}

function getBucketsColl(db, coll) {
    const bucketsColl = db["system.buckets." + coll.getName()];
    return bucketsColl;
}

function assertExistsAndTypeIs(coll, expectedType) {
    assert.neq(null, coll.exists());
    assert.eq(expectedType, coll.exists().type);
}

function assertDoesntExist(coll) {
    assert.eq(null, coll.exists());
}

// Drop database at the start to ensure a clean state.
assert.commandWorked(db.runCommand({dropDatabase: 1}));

// We have the following conditions:
//
// drop called on buckets
// drop called on main nss
//
// main nss
//     does not exist
//     normal collection
//     view on the bucket
//     view on another nss
//
// bucket
//     exists
//     does not exist
//
// That makes 16 permutations, however three permutations are not tested: the two where neither the
// main nss nor the buckets collection exist, and a normal collection without a bucket dropped by
// the main NSS (this is just dropping a normal collection).

jsTest.log("Case 1: normal timeseries, drop by the main NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createCollection(coll.getName(), timeseriesOptions));

    assertExistsAndTypeIs(coll, "timeseries");
    assertExistsAndTypeIs(bucketsColl, "collection");

    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    assertDoesntExist(coll);
    assertDoesntExist(bucketsColl);
}

jsTest.log("Case 2: normal timeseries, drop by the buckets NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createCollection(coll.getName(), timeseriesOptions));

    assertExistsAndTypeIs(coll, "timeseries");
    assertExistsAndTypeIs(bucketsColl, "collection");

    assert.commandWorked(db.runCommand({drop: bucketsColl.getName()}));

    assertDoesntExist(coll);
    assertDoesntExist(bucketsColl);
}

jsTest.log("Case 3: view on buckets, buckets doesn't exist, drop by the main NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createView(coll.getName(), bucketsColl.getName(), []));

    assertDoesntExist(bucketsColl);
    // Reported as "timeseries", even though the buckets collection doesn't exist.
    assertExistsAndTypeIs(coll, "timeseries");

    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    assertDoesntExist(coll);
    assertDoesntExist(bucketsColl);
}

jsTest.log("Case 4: view on buckets, buckets doesn't exist, drop by the buckets NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createView(coll.getName(), bucketsColl.getName(), []));

    assertDoesntExist(bucketsColl);
    // Reported as "timeseries", even though the buckets collection doesn't exist.
    assertExistsAndTypeIs(coll, "timeseries");

    assert.commandWorked(db.runCommand({drop: bucketsColl.getName()}));

    // The view still exists, since it's not technically a timeseries.
    assertExistsAndTypeIs(coll, "timeseries");
    assertDoesntExist(bucketsColl);
}

jsTest.log("Case 5: view on another collection, buckets exists, drop by the main NS");
// The create collection coordinator disallows the creation of the buckets collection or the view
// (depending on the creation order), so this case doesn't work for tracked collections.
if (!TestData.implicitlyTrackUnshardedCollectionOnCreation && !TestData.runningWithBalancer) {
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createView(coll.getName(), dummyCollection, []));
    assert.commandWorked(db.createCollection(bucketsColl.getName(), timeseriesOptions));

    assertExistsAndTypeIs(coll, "view");
    assertExistsAndTypeIs(bucketsColl, "collection");

    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    assertDoesntExist(coll);
    assertExistsAndTypeIs(bucketsColl, "collection");
} else {
    // Still get collection to advance counter.
    getNewColl(db);
    jsTest.log("Skipping due to implicitly tracking collections upon creation");
}

jsTest.log("Case 6: view on another collection, buckets exists, drop by the buckets NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createView(coll.getName(), dummyCollection, []));
    assert.commandWorked(db.createCollection(bucketsColl.getName(), timeseriesOptions));

    assertExistsAndTypeIs(coll, "view");
    assertExistsAndTypeIs(bucketsColl, "collection");

    assert.commandWorked(db.runCommand({drop: bucketsColl.getName()}));

    assertExistsAndTypeIs(coll, "view");
    assertDoesntExist(bucketsColl);
}

jsTest.log("Case 7: view on another collection, buckets doesn't exist, drop by the main NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createView(coll.getName(), dummyCollection, []));

    assertExistsAndTypeIs(coll, "view");
    assertDoesntExist(bucketsColl);

    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    assertDoesntExist(coll);
    assertDoesntExist(bucketsColl);
}

jsTest.log("Case 8: view on another collection, buckets doesn't exist, drop by the buckets NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createView(coll.getName(), dummyCollection, []));

    assertExistsAndTypeIs(coll, "view");
    assertDoesntExist(bucketsColl);

    assert.commandWorked(db.runCommand({drop: bucketsColl.getName()}));

    assertExistsAndTypeIs(coll, "view");
    assertDoesntExist(bucketsColl);
}

jsTest.log("Case 9: normal collection, buckets exists, drop by the main NS");
// TODO SERVER-95267: execute unconditionally.
if (!TestData.implicitlyTrackUnshardedCollectionOnCreation && !TestData.runningWithBalancer) {
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createCollection(coll.getName()));
    assert.commandWorked(db.createCollection(bucketsColl.getName(), timeseriesOptions));

    assertExistsAndTypeIs(coll, "collection");
    assertExistsAndTypeIs(bucketsColl, "collection");

    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    assertDoesntExist(coll);
    assertExistsAndTypeIs(bucketsColl, "collection");
} else {
    // Still get collection to advance counter.
    getNewColl(db);
    jsTest.log("Skipping due to implicitly tracking collections upon creation");
}

jsTest.log("Case 10: normal collection, buckets exists, drop by the buckets NS");
// TODO SERVER-95267: execute unconditionally.
if (!TestData.implicitlyTrackUnshardedCollectionOnCreation && !TestData.runningWithBalancer) {
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createCollection(coll.getName()));
    assert.commandWorked(db.createCollection(bucketsColl.getName(), timeseriesOptions));

    assertExistsAndTypeIs(coll, "collection");
    assertExistsAndTypeIs(bucketsColl, "collection");

    assert.commandWorked(db.runCommand({drop: bucketsColl.getName()}));

    assertExistsAndTypeIs(coll, "collection");
    assertDoesntExist(bucketsColl);
} else {
    // Still get collection to advance counter.
    getNewColl(db);
    jsTest.log("Skipping due to implicitly tracking collections upon creation");
}

jsTest.log("Case 11: normal collection, buckets doesn't exist, drop by the buckets NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createCollection(coll.getName()));

    assertExistsAndTypeIs(coll, "collection");
    assertDoesntExist(bucketsColl);

    assert.commandWorked(db.runCommand({drop: bucketsColl.getName()}));

    assertExistsAndTypeIs(coll, "collection");
    assertDoesntExist(bucketsColl);
}

jsTest.log("Case 12: timeseries without view, drop by the main NS");
// TODO SERVER-95267: execute unconditionally.
if (!TestData.implicitlyTrackUnshardedCollectionOnCreation && !TestData.runningWithBalancer) {
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createCollection(bucketsColl.getName(), timeseriesOptions));

    assertDoesntExist(coll);
    assertExistsAndTypeIs(bucketsColl, "collection");

    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    assertDoesntExist(coll);
    assertDoesntExist(bucketsColl);
} else {
    // Still get collection to advance counter.
    getNewColl(db);
    jsTest.log("Skipping due to implicitly tracking collections upon creation");
}

jsTest.log("Case 13: timeseries without view, drop by the buckets NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createCollection(bucketsColl.getName(), timeseriesOptions));

    assertDoesntExist(coll);
    assertExistsAndTypeIs(bucketsColl, "collection");

    assert.commandWorked(db.runCommand({drop: bucketsColl.getName()}));

    assertDoesntExist(coll);
    assertDoesntExist(bucketsColl);
}

// Also drop database at the end. This test creates inconsistent timeseries collections, which can
// break suites with add/remove shard.
assert.commandWorked(db.runCommand({dropDatabase: 1}));
