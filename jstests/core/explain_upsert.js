// Test explain for {upsert: true} updates.

var t = db.jstests_explain_upsert;
t.drop();

var explain;

// Explained upsert against an empty collection should succeed and be a no-op.
explain = db.runCommand(
    {explain: {update: t.getName(), updates: [{q: {a: 1}, u: {a: 1}, upsert: true}]}});
assert.commandWorked(explain);

// Collection should still not exist.
assert.eq(0, t.count());
assert(!t.drop());

// Add a document to the collection.
t.insert({a: 3});

// An explained upsert against a non-empty collection should also succeed as a no-op.
explain = db.runCommand(
    {explain: {update: t.getName(), updates: [{q: {a: 1}, u: {a: 1}, upsert: true}]}});
assert.commandWorked(explain);
assert.eq(1, t.count());
