// unique index constraint test for updates
// case where object doesn't grow tested here

t = db.indexa;
t.drop();

t.ensureIndex({x: 1}, true);

t.insert({'x': 'A'});
t.insert({'x': 'B'});
t.insert({'x': 'A'});

assert.eq(2, t.count(), "indexa 1");

t.update({x: 'B'}, {x: 'A'});

a = t.find().toArray();
u = Array.unique(a.map(function(z) {
    return z.x;
}));
assert.eq(2, t.count(), "indexa 2");

assert(a.length == u.length, "unique index update is broken");
