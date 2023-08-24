// Tests for $elemMatch to ensure correct count of returned elements.
// @tags: [
//   assumes_write_concern_unchanged,
// ]

const coll = db.c;
coll.drop();

assert.commandWorked(coll.createIndex({"arr.a": 1, "c": 1, "d": 1}));

assert.commandWorked(coll.insert({_id: 0, arr: [{a: 1}, {a: 2}], c: 4, d: 5, a: 1}));
assert.commandWorked(
    coll.insert(Array.from({length: 100}, () => ({arr: [{a: 99}], c: 99, d: 99}))));

// Test inequality inside $elemmatch.
const res = coll.explain("executionStats")
                .find({arr: {$elemMatch: {a: {$ne: 1}}}, $or: [{c: 4, d: 5}, {c: 6, d: 7}]})
                .finish();
assert.eq(1, res.executionStats.nReturned);
