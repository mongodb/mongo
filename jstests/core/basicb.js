
t = db.basicb;
t.drop();

assert.throws(function() {
    t.insert({'$a': 5});
});
