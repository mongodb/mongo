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

function assertExistsAndTypeIs(coll, expectedType) {
    assert.neq(null, coll.exists());
    assert.eq(expectedType, coll.exists().type);
}

function assertDoesntExist(coll) {
    assert.eq(null, coll.exists());
}

// Drop database at the start to ensure a clean state.
assert.commandWorked(db.runCommand({dropDatabase: 1}));

jsTest.log.info("normal timeseries, drop by the main NS");
{
    let coll = getNewColl(db);
    assert.commandWorked(db.createCollection(coll.getName(), timeseriesOptions));
    assertExistsAndTypeIs(coll, "timeseries");
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertDoesntExist(coll);
}

jsTest.log.info("view on buckets, buckets doesn't exist, drop by the main NS");
{
    let coll = getNewColl(db);
    const bucketsColl = db["system.buckets." + coll.getName()];
    assert.commandWorked(db.createView(coll.getName(), bucketsColl.getName(), []));
    assertDoesntExist(bucketsColl);
    // Reported as "timeseries", even though the buckets collection doesn't exist.
    assertExistsAndTypeIs(coll, "timeseries");
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertDoesntExist(coll);
    assertDoesntExist(bucketsColl);
}

jsTest.log.info("view on another collection, buckets doesn't exist, drop by the main NS");
{
    let coll = getNewColl(db);
    assert.commandWorked(db.createView(coll.getName(), dummyCollection, []));
    assertExistsAndTypeIs(coll, "view");
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertDoesntExist(coll);
}
