var col = db.memoryTest;

// test creating many collections to make sure no internal cache goes OOM
for (var i = 0; i < 10000; ++i) {
    name = "memoryTest" + i;
    if ((i % 1000) == 0) print("Processing " + name);
    db.eval(function(col) { for (var i = 0; i < 100; ++i) {db[col + "_" + i].find();} }, name);
}

// test recovery of JS engine after out of memory
db.system.js.save( { "_id" : "f1", "value" : function(n) {
    a = [];
    b = [];
    c = [];
    for (i = 0; i < n; i++) {
        a.push(Math.random());
        b.push(Math.random());
        c.push(Math.random());
    }
} })

// do mix of calls to make sure OOM is handled with no permanent damage
db.eval("f1(10)");
assert.throws(function() { db.eval("f1(100000000)"); } );
db.eval("f1(10)");
assert.throws(function() { db.eval("f1(1000000000)"); } );
db.eval("f1(1000000)");
db.eval("f1(1000000)");
db.eval("f1(1000000)");
assert.throws(function() { db.eval("f1(100000000)"); } );
db.eval("f1(10)");
db.eval("f1(1000000)");
db.eval("f1(1000000)");
db.eval("f1(1000000)");

// also test $where
col.drop();
col.insert({a: 1});
col.findOne({$where: "var arr = []; for (var i = 0; i < 1000000; ++i) {arr.push(0);}"});
assert.throws(function() { col.findOne({$where: "var arr = []; for (var i = 0; i < 1000000000; ++i) {arr.push(0);}"}); });
col.findOne({$where: "var arr = []; for (var i = 0; i < 1000000; ++i) {arr.push(0);}"});

