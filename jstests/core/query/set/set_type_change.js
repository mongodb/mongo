/**
 * Tests that using the $set update modifier to change only the type of a field will actually update
 * the document, including any relevant indices.
 */
let coll = db.set_type_change;
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.insert({a: 2}));

let newVal = new NumberLong(2);
let res = coll.update({}, {$set: {a: newVal}});
assert.eq(res.nMatched, 1);
assert.eq(res.nModified, 1);

// Make sure it actually changed the type.
let updated = coll.findOne();
assert(updated.a instanceof NumberLong, "$set did not update type of value: " + updated.a);
