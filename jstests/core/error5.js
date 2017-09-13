
t = db.error5;
t.drop();

assert.throws(function() {
    t.save(4);
    printjson(t.findOne());
}, [], "A");
t.save({a: 1});
assert.eq(1, t.count(), "B");
