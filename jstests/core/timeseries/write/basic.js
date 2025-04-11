/**
 * @tags: [
 *   requires_timeseries,
 *   known_query_shape_computation_problem,  # TODO (SERVER-103069): Remove this tag.
 *   requires_fastcount,
 * ]
 */

import {
    getTimeseriesCollForRawOps,
    kRawOperationSpec
} from "jstests/core/libs/raw_operation_utils.js";

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

// Fetch documents with find
docs = coll.find().toArray();
assert.eq(numDocs,
          docs.length,
          `Unexpected number of documents found with find
${tojson(docs)}`);

assert.eq(numDocs, coll.distinct('_id').length);
assert.eq(numDocs, coll.count());

function testRawDataQueries() {
    let bucketsDocs =
        getTimeseriesCollForRawOps(coll).aggregate([{$match: {}}], kRawOperationSpec).toArray();
    // There's no guarantee how many raw documents exist before unpacking. We only assert
    // consistency across commands with rawData: true.
    const numBucketDocs = bucketsDocs.length;
    validateBuckets(bucketsDocs, numDocs);

    bucketsDocs = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
    validateBuckets(bucketsDocs, numDocs);

    // A predicate which matches only bucket documents, not unpacked documents.
    const bucketPredicate = {"control.count": {$exists: true}};

    assert.eq(0, coll.distinct('_id', bucketPredicate));
    assert.eq(numBucketDocs,
              getTimeseriesCollForRawOps(coll)
                  .distinct('_id', bucketPredicate, kRawOperationSpec)
                  .length);

    // Should return the same count as distinct().length.
    assert.eq(numBucketDocs,
              getTimeseriesCollForRawOps(coll).count(bucketPredicate, kRawOperationSpec));
    // No unpacked documents should match the bucket structure.
    assert.eq(0, coll.count(bucketPredicate));
}

testRawDataQueries();
