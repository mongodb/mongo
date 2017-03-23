
t = db.geoa;
t.drop();

t.save({_id: 1, a: {loc: [5, 5]}});
t.save({_id: 2, a: {loc: [6, 6]}});
t.save({_id: 3, a: {loc: [7, 7]}});

t.ensureIndex({"a.loc": "2d"});

cur = t.find({"a.loc": {$near: [6, 6]}});
assert.eq(2, cur.next()._id, "A1");
