var t = db.array_match4;

t.drop();
t.save({a: [1, 2]});

var query_gte = {a: {$gte: [1, 2]}};

//
// without index
//

assert.eq(1, t.find({a: [1, 2]}).count(), '$eq (without index)');
assert.eq(1, t.find(query_gte).itcount(), '$gte (without index)');

//
// with index
//

t.ensureIndex({a: 1});
assert.eq(1, t.find({a: [1, 2]}).count(), '$eq (with index)');

// display explain output (for index bounds)
var explain = t.find(query_gte).explain();
print('explain for ' + tojson(query_gte, '', true) + ' = ' + tojson(explain));

// number of documents returned by indexes query should be consistent
// with non-indexed case.
// XXX: The following assertion documents current behavior.
// XXX: 2.4 and 2.6 both return 0 documents.
assert.eq(0, t.find(query_gte).itcount(), '$gte (with index)');
