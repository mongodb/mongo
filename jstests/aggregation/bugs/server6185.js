// projecting a non-existent subfield should work as it does in a query with projection
c = db.c;
c.drop();

c.save({a: [1]});
c.save({a: {c: 1}});
c.save({a: [{c: 1}, {b: 1, c: 1}, {c: 1}]});
c.save({a: 1});
c.save({b: 1});

// assert the aggregation and the query produce the same thing
assert.eq(c.aggregate({$project: {'a.b': 1}}).toArray(), c.find({}, {'a.b': 1}).toArray());
