
a = [1, "asd", null, [2, 3], new Date(), {x: 1}];

for (var i = 0; i < a.length; i++) {
    var ret = db.eval("function( a , i ){ return a[i]; }", a, i);
    assert.eq(typeof(a[i]), typeof(ret), "type test");
    assert.eq(a[i], ret, "val test: " + typeof(a[i]));
}

db.eval9.drop();
db.eval9.save({a: 17});

assert.eq(1, db.eval("return db.eval9.find().toArray()").length, "A");
assert.eq(17, db.eval("return db.eval9.find().toArray()")[0].a, "B");

// just to make sure these things don't crash (but may throw an exception)
try {
    db.eval("return db.eval9.find()");
    db.eval("return db.eval9");
    db.eval("return db");
    db.eval("return print");
} catch (ex) {
}