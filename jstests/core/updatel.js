// The positional operator allows an update modifier field path to contain a sentinel ('$') path
// part that is replaced with the numeric position of an array element matched by the update's query
// spec.  <http://dochub.mongodb.org/core/positionaloperator>

// If no array element position from a query is available to substitute for the positional operator
// setinel ('$'), the update fails with an error.  SERVER-6669 SERVER-4713

var res;
t = db.jstests_updatel;
t.drop();

// The collection is empty, forcing an upsert.  In this case the query has no array position match
// to substiture for the positional operator.  SERVER-4713
assert.writeError(t.update({}, {$set: {'a.$.b': 1}}, true));
assert.eq(0, t.count(), "No upsert occurred.");

// Save a document to the collection so it is no longer empty.
assert.writeOK(t.save({_id: 0}));

// Now, with an existing document, trigger an update rather than an upsert.  The query has no array
// position match to substiture for the positional operator.  SERVER-6669
assert.writeError(t.update({}, {$set: {'a.$.b': 1}}));
assert.eq([{_id: 0}], t.find().toArray(), "No update occurred.");

// Now, try with an update by _id (without a query array match).
assert.writeError(t.update({_id: 0}, {$set: {'a.$.b': 1}}));
assert.eq([{_id: 0}], t.find().toArray(), "No update occurred.");

// Seed the collection with a document suitable for the following check.
assert.writeOK(t.remove({}));
assert.writeOK(t.save({_id: 0, a: [{b: {c: 1}}]}));

// Now, attempt to apply an update with two nested positional operators.  There is a positional
// query match for the first positional operator but not the second.  Note that dollar sign
// substitution for multiple positional opertors is not implemented (SERVER-831).
assert.writeError(t.update({'a.b.c': 1}, {$set: {'a.$.b.$.c': 2}}));
assert.eq([{_id: 0, a: [{b: {c: 1}}]}], t.find().toArray(), "No update occurred.");

// SERVER-1155 test an update with the positional operator
// that has a regex in the query field
t.drop();
assert.writeOK(t.insert({_id: 1, arr: [{a: "z", b: 1}]}));
assert.writeOK(t.update({"arr.a": /^z$/}, {$set: {"arr.$.b": 2}}, false, true));
assert.eq(t.findOne().arr[0], {a: "z", b: 2});

t.drop();
assert.writeOK(t.insert({_id: 1, arr: [{a: "z", b: 1}, {a: "abc", b: 2}, {a: "lmn", b: 3}]}));
assert.writeOK(t.update({"arr.a": /l/}, {$inc: {"arr.$.b": 2}}, false, true));
assert.eq(t.findOne().arr[2], {a: "lmn", b: 5});

// Test updates with ambiguous positional operator.
t.drop();
assert.writeOK(t.insert({_id: 0, a: [1, 2]}));
assert.writeError(t.update({$and: [{a: 1}, {a: 2}]}, {$set: {'a.$': 5}}));
assert.eq([{_id: 0, a: [1, 2]}], t.find().toArray(), "No update occurred.");

t.drop();
assert.writeOK(t.insert({_id: 0, a: [1], b: [2]}));
assert.writeError(t.update({a: 1, b: 2}, {$set: {'a.$': 5}}));
assert.eq([{_id: 0, a: [1], b: [2]}], t.find().toArray(), "No update occurred.");

t.drop();
assert.writeOK(t.insert({_id: 0, a: [1], b: [2]}));
assert.writeError(t.update({a: {$elemMatch: {$lt: 2}}, b: 2}, {$set: {'a.$': 5}}));
assert.eq([{_id: 0, a: [1], b: [2]}], t.find().toArray(), "No update occurred.");

t.drop();
assert.writeOK(t.insert({_id: 0, a: [{b: 1}, {c: 2}]}));
assert.writeError(t.update({'a.b': 1, 'a.c': 2}, {$set: {'a.$': 5}}));
assert.eq([{_id: 0, a: [{b: 1}, {c: 2}]}], t.find().toArray(), "No update occurred.");