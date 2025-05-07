// @tags: [
//     requires_getmore,
//     # This test relies on query commands returning specific batch-sized responses.
//     assumes_no_implicit_cursor_exhaustion,
// ]

// Test that the MaxBytesToReturnToClientAtOnce limit as set in 'kMaxBytesToReturnToClientAtOnce' is
// enforced. The size of the result should not exceed the 'findCommandBatchSize' or this limit,
// whichever produces fewer documents.

let t = db.jstests_find9;
t.drop();

const findCommandBatchSize = assert.commandWorked(db.adminCommand(
    {getParameter: 1, internalQueryFindCommandBatchSize: 1}))["internalQueryFindCommandBatchSize"];
const kNumDocs = 60;

let big = new Array(500000).toString();
for (let i = 0; i < kNumDocs; ++i) {
    assert.commandWorked(t.insertOne({a: i, b: big}));
}

// Check size limit with a simple query.
assert.eq(Math.min(findCommandBatchSize, kNumDocs),
          t.find({}, {a: 1}).objsLeftInBatch());  // Projection resizes the result set.
assert.gt(kNumDocs, t.find().objsLeftInBatch());

// Check size limit on a query with an explicit batch size.
assert.eq(60, t.find({}, {a: 1}).batchSize(80).objsLeftInBatch());
assert.gt(60, t.find().batchSize(80).objsLeftInBatch());

for (let i = 0; i < kNumDocs; ++i) {
    assert.commandWorked(t.insertOne({a: i, b: big}));
}

// Check size limit with get more.
let c = t.find().batchSize(80);
while (c.hasNext()) {
    assert.gt(kNumDocs, c.objsLeftInBatch());
    c.next();
}

assert.eq(2 * kNumDocs, t.find().sort({$natural: 1}).itcount());
assert.eq(2 * kNumDocs, t.find().sort({_id: 1}).itcount());
