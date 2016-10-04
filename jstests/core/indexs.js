// Test index key generation issue with parent and nested fields in same index and array containing
// subobject SERVER-3005.

t = db.jstests_indexs;

t.drop();
t.ensureIndex({a: 1});
t.save({a: [{b: 3}]});
assert.eq(1, t.count({a: {b: 3}}));

t.drop();
t.ensureIndex({a: 1, 'a.b': 1});
t.save({a: {b: 3}});
assert.eq(1, t.count({a: {b: 3}}));

t.drop();
t.ensureIndex({a: 1, 'a.b': 1});
t.save({a: [{b: 3}]});
assert.eq(1, t.count({a: {b: 3}}));
