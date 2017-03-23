// Disallow $ in field names
var res;

t = db.jstest_updateh;
t.drop();

t.insert({x: 1});

res = t.update({x: 1}, {$set: {y: 1}});  // ok
assert.writeOK(res);

res = t.update({x: 1}, {$set: {$z: 1}});  // not ok
assert.writeError(res);

res = t.update({x: 1}, {$set: {'a.$b': 1}});  // not ok
assert.writeError(res);

res = t.update({x: 1}, {$inc: {$z: 1}});  // not ok
assert.writeError(res);

res = t.update({x: 1}, {$pushAll: {$z: [1, 2, 3]}});  // not ok
assert.writeError(res);

// Second section
t.drop();

t.save({_id: 0, n: 0});

// Test that '$' cannot be the first character in a field.
// SERVER-7150
res = t.update({n: 0}, {$set: {$x: 1}});
assert.writeError(res);

res = t.update({n: 0}, {$set: {$$$: 1}});
assert.writeError(res);

res = t.update({n: 0}, {$set: {"sneaky.$x": 1}});
assert.writeError(res);

res = t.update({n: 0}, {$set: {"secret.agent$.$x": 1}});
assert.writeError(res);

res = t.update({n: 0}, {$set: {"$secret.agent.x": 1}});
assert.writeError(res);

res = t.update({n: 0}, {$set: {"secret.agent$": 1}});
assert.writeOK(res);
t.save({_id: 0, n: 0});

// Test that you cannot update database references into top level fields
// Enable after SERVER-14252 fixed: currently validation does not catch DBRef
// fields at the top level for update and will not cause an error here
// res = t.update({ n: 0 }, { $set: {$ref: "1", $id: 1, $db: "1"}});
// assert.writeError(res);

// res = t.update({ n: 0 }, { $set: {$ref: "1", $id: 1}});
// assert.writeError(res);

// SERVER-11241: Validation used to allow any DBRef field name as a prefix
// thus allowing things like $idXXX
res = t.update({n: 0}, {$set: {$reffoo: 1}});
assert.writeError(res);

res = t.update({n: 0}, {$set: {$idbar: 1}});
assert.writeError(res);

res = t.update({n: 0}, {$set: {$dbbaz: 1}});
assert.writeError(res);

// Test that '$id', '$db', and '$ref' are acceptable field names in
// the correct case ( subdoc)
// SERVER-3231
res = t.update({n: 0}, {$set: {'x.$ref': '1', 'x.$id': 1, 'x.$db': '1'}});
assert.writeOK(res);
t.save({_id: 0, n: 0});

// Test that '$' can occur elsewhere in a field name.
// SERVER-7557
res = t.update({n: 0}, {$set: {ke$sha: 1}});
assert.writeOK(res);
t.save({_id: 0, n: 0});

res = t.update({n: 0}, {$set: {more$$moreproblem$: 1}});
assert.writeOK(res);
t.save({_id: 0, n: 0});
