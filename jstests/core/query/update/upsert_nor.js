/**
 * Tests that we cannot extract document fields behind $not from the query to use them to upsert
 * documents.
 *
 * Long story: 'update' operation with 'upsert: true' upserts a new document if the filter does
 * not match any documents. The new document is the combination of fields from the filter and the
 * update expressions. The query engine ignores any fields in the filter which are behind negations
 * ($nor, $not). However, the Boolean simplifier, introduced in 7.3, can simplify such expressions
 * and remove some negations.
 * Let's take a look at the filter expression with root $nor:
 * '{$nor: [{$or: [{"x": 1}, {"y": 2}]}, {"x": {$ne: 3}}]}'
 * since everything is behind $nor the query engine cannot extract any field values for the document
 * to upsert. However, the Boolean simplifier will simplify such expression to
 * '{x: {$eq: 3, $ne: 1}, y: {$ne: 2}}'.
 * Now the query engine can extract '{x: 3}' for the document to upsert. This brings the issue of
 * inconsistent behaviour depending on whether the simplifier enabled or not, which is incorrect
 * since the simplifier shouldn't change the query's semantics. We decided to disable the simplifier
 * for upsert operations until we come up the better solution.
 *
 * @tags: [assumes_unsharded_collection, requires_non_retryable_writes]
 */

const collectionName = jsTestName();
const coll = db[collectionName];
coll.drop();

// This filter can be simplified to {x: {$eq: 3, $ne: 1}, y: {$ne: 2}}.
const filter = {
    $nor: [{$or: [{"x": 1}, {"y": 2}]}, {"x": {$ne: 3}}]
};

const upsertResult =
    assert.commandWorked(coll.updateOne(filter, [{"$unset": "z"}], {upsert: true}));

// Make sure that we upserted the document.
assert(upsertResult.upsertedId);

// Make sure that the Boolean simplifier was disabled and we didn't manage to extract {x: 3} from
// the query.
const foundDocuments = coll.find({x: 3}).toArray();
assert.eq(0, foundDocuments.length);
