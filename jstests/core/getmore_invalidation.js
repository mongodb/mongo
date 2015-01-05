// Tests for invalidation during a getmore. This behavior is storage-engine dependent.
// See SERVER-16675.

var t = db.jstests_getmore_invalidation;

// Case #1: Text search with deletion invalidation.
t.drop();
assert.commandWorked(t.ensureIndex({a: "text"}));
assert.writeOK(t.insert({_id: 1, a: "bar"}));
assert.writeOK(t.insert({_id: 2, a: "bar"}));
assert.writeOK(t.insert({_id: 3, a: "bar"}));

var cursor = t.find({$text: {$search: "bar"}}).batchSize(2);
cursor.next();
cursor.next();

assert.writeOK(t.remove({_id: 3}));

// We should get back the document or not (depending on the storage engine / concurrency model).
// Either is fine as long as we don't crash.
var count = cursor.itcount();
assert(count === 0 || count === 1);

// Case #2: Text search with mutation invalidation.
t.drop();
assert.commandWorked(t.ensureIndex({a: "text"}));
assert.writeOK(t.insert({_id: 1, a: "bar"}));
assert.writeOK(t.insert({_id: 2, a: "bar"}));
assert.writeOK(t.insert({_id: 3, a: "bar"}));

var cursor = t.find({$text: {$search: "bar"}}).batchSize(2);
cursor.next();
cursor.next();

// Update the next matching doc so that it no longer matches.
assert.writeOK(t.update({_id: 3}, {$set: {a: "nomatch"}}));

// Either the cursor should skip the result that no longer matches, or we should get back the old
// version of the doc.
assert(!cursor.hasNext() || cursor.next()["a"] === "bar");

// Case #3: Merge sort with deletion invalidation.
t.drop();
assert.commandWorked(t.ensureIndex({a: 1, b: 1}));
assert.writeOK(t.insert({a: 1, b: 1}));
assert.writeOK(t.insert({a: 1, b: 2}));
assert.writeOK(t.insert({a: 2, b: 3}));
assert.writeOK(t.insert({a: 2, b: 4}));

var cursor = t.find({a: {$in: [1,2]}}).sort({b: 1}).batchSize(2);
cursor.next();
cursor.next();

assert.writeOK(t.remove({a: 2, b: 3}));

var count = cursor.itcount();
assert(count === 1 || count === 2);

// Case #4: Merge sort with mutation invalidation.
t.drop();
assert.commandWorked(t.ensureIndex({a: 1, b: 1}));
assert.writeOK(t.insert({a: 1, b: 1}));
assert.writeOK(t.insert({a: 1, b: 2}));
assert.writeOK(t.insert({a: 2, b: 3}));
assert.writeOK(t.insert({a: 2, b: 4}));

var cursor = t.find({a: {$in: [1,2]}}).sort({b: 1}).batchSize(2);
cursor.next();
cursor.next();

assert.writeOK(t.update({a: 2, b: 3}, {$set: {a: 6}}));

// Either the cursor should skip the result that no longer matches, or we should get back the old
// version of the doc.
assert(cursor.hasNext());
assert(cursor.next()["a"] === 2);
if (cursor.hasNext()) {
    assert(cursor.next()["a"] === 2);
}
assert(!cursor.hasNext());
