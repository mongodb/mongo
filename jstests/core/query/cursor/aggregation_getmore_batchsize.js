// @tags: [
//     requires_getmore,
//     # This test relies on aggregate commands returning specific batch-sized responses.
//     assumes_no_implicit_cursor_exhaustion,
// ]

// this is a simple test to make sure that batchSize gets propagated to the cursors returned
// from the aggregate sell helper

let coll = db["aggregation_getmore_batchsize"];

// Insert some data to query for
assert.commandWorked(coll.insert([{a: 1}, {a: 1}, {a: 1}, {a: 1}, {a: 1}, {a: 1}]));

// Create a cursor with a batch size of 2 (should require three full batches to return all
// documents).
let cursor = coll.aggregate([{$match: {a: 1}}, {$limit: 6}], {cursor: {batchSize: 2}});
let curCount = 2;

// Check that each batch has only two documents in it.
for (let i = 0; i < 6; i++) {
    print(tojson(cursor.next()));
    jsTestLog("Expecting " + (curCount - 1));
    assert.eq(cursor.objsLeftInBatch(), --curCount);
    if (curCount == 0) curCount = 2;
}

// Create a cursor with a batch size of 0 (should only return one full batch of documents).
// {batchSize: 0} is a special case where the server will return a cursor ID immediately, or
// an error, but the first document result will be fetched by a getMore.
cursor = coll.aggregate([{$match: {a: 1}}, {$limit: 6}], {cursor: {batchSize: 0}});
assert.eq(cursor.objsLeftInBatch(), 0);
print(tojson(cursor.next()));
assert.eq(cursor.objsLeftInBatch(), 5);

// Check that the default cursor behavior works if you specify a cursor but no batch size.
cursor = coll.aggregate([{$match: {a: 1}}, {$limit: 6}], {cursor: {}});
assert.eq(cursor.objsLeftInBatch(), 6);
