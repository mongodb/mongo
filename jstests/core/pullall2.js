
t = db.pullall2;
t.drop();

o = {
    _id: 1,
    a: []
};
for (i = 0; i < 5; i++)
    o.a.push({x: i, y: i});

t.insert(o);

assert.eq(o, t.findOne(), "A");

t.update({}, {$pull: {a: {x: 3}}});
o.a = o.a.filter(function(z) {
    return z.x != 3;
});
assert.eq(o, t.findOne(), "B");

t.update({}, {$pull: {a: {x: {$in: [1, 4]}}}});
o.a = o.a.filter(function(z) {
    return z.x != 1;
});
o.a = o.a.filter(function(z) {
    return z.x != 4;
});
assert.eq(o, t.findOne(), "C");
