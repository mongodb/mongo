/**
 * Tests that time-series bucket OIDs are generated, and collisions are handled, as expected.
 */

function incrementOID(oid) {
    const prefix = oid.toString().substr(10, 16);
    const suffix = oid.toString().substr(26, 8);

    const number = parseInt(suffix, 16) + 1;
    const incremented = number.toString(16).padStart(8, '0');

    return ObjectId(prefix + incremented);
}

const conn = MongoRunner.runMongod();

const testDB = conn.getDB(jsTestName());

const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';
const metaFieldName = 'meta';

const runTest = (ordered) => {
    coll.drop();
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    // Insert a couple measurements in different buckets.
    assert.commandWorked(
        coll.insert({[timeFieldName]: ISODate("2023-08-01T00:00:00.000Z"), [metaFieldName]: 1}),
        {ordered});
    assert.commandWorked(
        coll.insert({[timeFieldName]: ISODate("2023-08-01T00:00:00.000Z"), [metaFieldName]: 2}),
        {ordered});

    // Check that we get consecutive OIDs.
    const id1 = bucketsColl.findOne({meta: 1})._id;
    const id2 = bucketsColl.findOne({meta: 2})._id;
    assert.eq(id2, incrementOID(id1));

    // Check that the numBucketsOpenedDueToMetadata metric is increased twice, once for
    // each bucket we created from inserting a measurement with a new metadata.
    let stats = assert.commandWorked(coll.stats());
    let expectedNumBucketsOpenedDueToMetadata = stats.timeseries['numBucketsOpenedDueToMetadata'];

    // Now directly insert a bogus bucket with the next sequential OID.
    const bogusBucket = bucketsColl.findOne({meta: 2});
    bogusBucket._id = incrementOID(id2);
    assert.commandWorked(bucketsColl.insert(bogusBucket));

    // Now insert another measurement that opens a new bucket and check that the ID is no longer
    // sequential.
    assert.commandWorked(coll.insert(
        {[timeFieldName]: ISODate("2023-08-01T00:00:00.000Z"), [metaFieldName]: 3}, {ordered}));
    const id3 = bucketsColl.findOne({meta: 3})._id;
    assert.neq(id3, incrementOID(id2));
    assert.neq(id3, incrementOID(incrementOID(id2)));

    // Check that the retry logic that handles bucket OID collision did not increment the
    // numBucketsOpenedDueToMetadata metric more than intended - in this case it should have only
    // been incremented once
    expectedNumBucketsOpenedDueToMetadata++;
    stats = assert.commandWorked(coll.stats());
    assert.eq(stats.timeseries['numBucketsOpenedDueToMetadata'],
              expectedNumBucketsOpenedDueToMetadata);
};

runTest(true);
runTest(false);

MongoRunner.stopMongod(conn);
