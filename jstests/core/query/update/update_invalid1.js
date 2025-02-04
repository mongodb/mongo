// @tags: [
//   requires_fastcount,
// ]

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.update({_id: 5}, {$set: {$inc: {x: 5}}}, true));

// From version 5.0 on field names with dots and dollars are enabled and only top-level $-prefixed
// fields are validated. The field '$inc' appears at a lower level than the operator $set, so it is
// accepted by the update validation.
assert.eq(1, t.count(), "A1");
