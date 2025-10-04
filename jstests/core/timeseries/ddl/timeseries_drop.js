/**
 * Test several conditions when dropping timeseries collections.
 *
 * @tags: [
 *   requires_timeseries,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

const timeseriesOptions = {
    timeseries: {timeField: "t"},
};

const dummyCollection = "dummyCollection";

let _collCounter = 1;
function getNewColl(db) {
    const collNamePrefix = jsTestName() + "_coll_";
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

jsTest.log("normal timeseries, drop by the main NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createCollection(coll.getName(), timeseriesOptions));

    assertExistsAndTypeIs(coll, "timeseries");
    assertDoesntExist(bucketsColl);

    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    assertDoesntExist(coll);
    assertDoesntExist(bucketsColl);
}

jsTest.log("normal timeseries, drop by the buckets NS");
{
    let coll = getNewColl(db);
    let bucketsColl = getBucketsColl(db, coll);

    assert.commandWorked(db.createCollection(coll.getName(), timeseriesOptions));

    assertExistsAndTypeIs(coll, "timeseries");
    assertDoesntExist(bucketsColl);

    assert.commandWorked(db.runCommand({drop: bucketsColl.getName()}));

    assertExistsAndTypeIs(coll, "timeseries");
    assertDoesntExist(bucketsColl);
}

jsTest.log("view on buckets, buckets doesn't exist, drop by the main NS");
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

jsTest.log("view on buckets, buckets doesn't exist, drop by the buckets NS");
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

jsTest.log("view on another collection, buckets doesn't exist, drop by the main NS");
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

jsTest.log("view on another collection, buckets doesn't exist, drop by the buckets NS");
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

jsTest.log("normal collection, buckets doesn't exist, drop by the buckets NS");
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
