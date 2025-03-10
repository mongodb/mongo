/**
 * @tags: [
 *   requires_timeseries,
 *   # TODO (SERVER-101293): Remove this tag.
 *   known_query_shape_computation_problem,
 *   featureFlagRawDataCrudOperations,
 * ]
 */

function validateBuckets(bucketsDocs, expectedLogicalDocNum) {
    let logicalDocNum = 0;
    bucketsDocs.forEach(doc => {
        assert.hasFields(doc, ['_id', 'control', 'data']);
        assert.hasFields(doc.control, ['version', 'min', 'max', 'count']);
        logicalDocNum += doc.control.count;
    });

    assert.eq(expectedLogicalDocNum,
              logicalDocNum,
              `Unexpected numnber of logical documents found by looking at buckets metadata ${
                  bucketsDocs}`);
}

const coll = db[jsTestName()];
coll.drop();

const timeFieldName = 'time';
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.contains(coll.getName(), db.getCollectionNames());

const numDocs = 3;

let toInsert = [];
for (let i = 0; i < numDocs; i++) {
    const t = ISODate();
    toInsert.push({_id: i, [timeFieldName]: t});
}

toInsert.forEach((doc) => coll.insertOne(doc));

// Fetch documents with aggregate
let docs = coll.aggregate([{$match: {}}]).toArray();
assert.eq(
    numDocs, docs.length, `Unexpected number of documents found with aggregate ${tojson(docs)}`);

// Count documents
assert.eq(numDocs, coll.countDocuments({}), `Unexpected number of documents with countDocuments`);

// TODO SERVER-101498 re-enable this test case once find path works for viewless timeseries
// Fetch documents with find
// docs = coll.find().toArray();
// assert.eq(numDocs, docs.length, `Unexpected number of documents found with find
// ${tojson(docs)}`);

let bucketsDocs = coll.aggregate([{$match: {}}], {rawData: true}).toArray();
validateBuckets(bucketsDocs, numDocs);

// TODO SERVER-101498 re-enable this test case once find path works for viewless timeseries
// bucketsDocs = coll.find().rawData().toArray();
// validateBuckets(bucketsDocs, numDocs);
