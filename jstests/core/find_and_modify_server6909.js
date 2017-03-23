c = db.find_and_modify_server6906;

c.drop();

c.insert({_id: 5, a: {b: 1}});
ret = c.findAndModify({
    query: {'a.b': 1},
    update: {$set: {'a.b': 2}},  // Ensure the query on 'a.b' no longer matches.
    new: true
});
assert.eq(5, ret._id);
assert.eq(2, ret.a.b);

c.drop();

c.insert({_id: null, a: {b: 1}});
ret = c.findAndModify({
    query: {'a.b': 1},
    update: {$set: {'a.b': 2}},  // Ensure the query on 'a.b' no longer matches.
    new: true
});
assert.eq(2, ret.a.b);
