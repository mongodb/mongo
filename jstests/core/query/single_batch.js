// Test the "single batch" semantics of negative limit.
// @tags: [
//   requires_getmore,
// ]
const coll = db.jstests_single_batch;
coll.drop();

// 1 MB.
const padding = "x".repeat(1024 * 1024);

// Insert ~20 MB of data.
for (let i = 0; i < 20; i++) {
    assert.commandWorked(coll.insert({_id: i, padding: padding}));
}

// The limit is 18, but we should end up with fewer documents since 18 docs won't fit in a
// single 16 MB batch.
const numResults = coll.find().limit(-18).itcount();
assert.lt(numResults, 18);
assert.gt(numResults, 0);
