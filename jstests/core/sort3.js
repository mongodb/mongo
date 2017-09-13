t = db.sort3;
t.drop();

t.save({a: 1});
t.save({a: 5});
t.save({a: 3});

assert.eq("1,5,3", t.find().toArray().map(function(z) {
    return z.a;
}));

assert.eq("1,3,5", t.find().sort({a: 1}).toArray().map(function(z) {
    return z.a;
}));
assert.eq("5,3,1", t.find().sort({a: -1}).toArray().map(function(z) {
    return z.a;
}));
