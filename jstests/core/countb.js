// Test fast count mode with single key index unsatisfiable constraints on a multi key index.

t = db.jstests_countb;
t.drop();

t.ensureIndex({a: 1});
t.save({a: ['a', 'b']});
assert.eq(0, t.find({a: {$in: ['a'], $gt: 'b'}}).count());
assert.eq(0, t.find({$and: [{a: 'a'}, {a: {$gt: 'b'}}]}).count());
assert.eq(1, t.find({$and: [{a: 'a'}, {$where: "this.a[1]=='b'"}]}).count());
assert.eq(0, t.find({$and: [{a: 'a'}, {$where: "this.a[1]!='b'"}]}).count());
