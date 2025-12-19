/**
 * Tests the conversion between viewful and viewless timeseries when upgrading or downgrading
 * the FCV across the viewless timeseries feature flag.
 * TODO(SERVER-114573): Remove this test once 9.0 becomes lastLTS.
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getTimeseriesBucketsColl} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

if (lastLTSFCV != "8.0") {
    print("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB(jsTestName());
const coll = db.myts;

// Create a viewless timeseries collection with two measurements going into the same bucket.
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
assert.commandWorked(coll.createIndex({foo: 1}));
assert.commandWorked(coll.insertOne({t: ISODate(), m: 123}));
assert.commandWorked(coll.insertOne({t: ISODate(), m: 123}));

const expectedViewlessMetadata = coll.getMetadata();
const expectedIndexes = coll.getIndexes();

// Check that the collection can be converted to viewful timeseries and back to viewless.
function assertValidTimeseriesCollection(nodeColl, {expectViewlessFormat}) {
    // The collection exists
    assert(nodeColl.exists());

    if (expectViewlessFormat) {
        assert(!getTimeseriesBucketsColl(nodeColl).exists());
    } else {
        assert(getTimeseriesBucketsColl(nodeColl).exists());
        assert(getTimeseriesBucketsColl(nodeColl).getMetadata().options.validator);
    }

    // We keep the same metadata and indexes
    if (expectViewlessFormat) {
        assert.docEq(expectedViewlessMetadata, nodeColl.getMetadata());
    } else {
        // In the case of viewful format, the UUID is detached to the system.buckets collection,
        // but the rest of the metadata matches that of the viewless format.
        const expectedViewfulMetadata = Object.extend({}, expectedViewlessMetadata, true /* deep */);
        delete expectedViewfulMetadata.info.uuid;
        assert.docEq(expectedViewfulMetadata, nodeColl.getMetadata());
        assert.eq(expectedViewlessMetadata.info.uuid, getTimeseriesBucketsColl(nodeColl).getUUID());
    }
    assert.docEq(expectedIndexes, nodeColl.getIndexes());

    // The data is still accessible
    assert.eq(2, nodeColl.countDocuments({}));
    assert.eq(1, nodeColl.countDocuments({}, {rawData: true}));
}

function assertValidTimeseriesCollectionInAllNodes({expectViewlessFormat}) {
    assertValidTimeseriesCollection(coll, {expectViewlessFormat});

    rst.awaitReplication();
    const secondaryColl = rst.getSecondary().getDB(db.getName()).getCollection(coll.getName());
    assertValidTimeseriesCollection(secondaryColl, {expectViewlessFormat});
}

assertValidTimeseriesCollectionInAllNodes({expectViewlessFormat: true});

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
assertValidTimeseriesCollectionInAllNodes({expectViewlessFormat: false});

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
assertValidTimeseriesCollectionInAllNodes({expectViewlessFormat: true});

rst.stopSet();
