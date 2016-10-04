
t = db.pull_remove1;
t.drop();

o = {
    _id: 1,
    a: [1, 2, 3, 4, 5, 6, 7, 8]
};
t.insert(o);

assert.eq(o, t.findOne(), "A1");

o.a = o.a.filter(function(z) {
    return z >= 6;
});
t.update({}, {$pull: {a: {$lt: 6}}});

assert.eq(o.a, t.findOne().a, "A2");
