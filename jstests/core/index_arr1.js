t = db.index_arr1;
t.drop();

t.insert({_id: 1, a: 5, b: [{x: 1}]});
t.insert({_id: 2, a: 5, b: []});
t.insert({_id: 3, a: 5});

assert.eq(3, t.find({a: 5}).itcount(), "A1");

t.ensureIndex({a: 1, "b.x": 1});

assert.eq(3, t.find({a: 5}).itcount(), "A2");  // SERVER-1082

assert.eq(2, t.getIndexes().length, "B1");
t.insert({_id: 4, a: 5, b: []});
t.ensureIndex({a: 1, "b.a": 1, "b.c": 1});
assert.eq(3, t.getIndexes().length, "B2");
