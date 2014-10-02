// SERVER-8514

var t = db.count11;

t.drop();

var query_good = {a: {$in: [null]}};
var query_bad = {a: {$in: null}};

// query non-existing collection
// returns 0 on valid syntax query
// fails on invalid syntax query
result = t.find(query_good).count();
assert.eq(0, result);
assert.throws(function() { t.find(query_bad).count(); });

// query itcount on non-existing collection
// returns 0 on valid syntax query
// fails on invalid syntax query
result = t.find(query_good).itcount();
assert.eq(0, result);
assert.throws(function() { t.find(query_bad).itcount(); });

// query existing but collection
// returns 0 on valid syntax query
// fails on invalid syntax query
db.createCollection("count11");
result = t.find(query_good).count();
assert.eq(0, result);
assert.throws(function() { t.find(query_bad).count(); });

// query non-empty collection
// returns 0 on valid syntax query
// fails on invalid syntax query
t.save({a: [1, 2]});
result = t.find(query_good).count();
assert.eq(0, result);
assert.throws(function() { t.find(query_bad).count(); });