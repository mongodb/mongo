/**
 * Test that $nor with children that cannot be rewritten doesn't raise an exception due to empty
 * $nor array. Exercises the fix for SERVER-78650.
 */

const collName = jsTestName();
const coll = db[collName];
coll.drop();

let watchCursor = coll.watch([{$match: {$nor: [{a: {$exists: true}}, {b: {$exists: true}}]}}]);
assert.commandWorked(coll.insertOne({_id: 1}));

assert.soon(() => watchCursor.hasNext());

const nextEvent = watchCursor.next();
assert.eq(nextEvent.operationType, "insert");
assert.eq(nextEvent.documentKey._id, 1);

watchCursor.close();
