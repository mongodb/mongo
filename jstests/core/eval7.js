
assert.eq(6, db.eval("5 + 1"), "A");
assert.throws(function(z) {
    db.eval("5 + function x; + 1");
});
