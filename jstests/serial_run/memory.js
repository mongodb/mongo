/*
 * @tags: [blacklist_from_ppc64le]
 */
var col = db.memoryTest;

var buildInfo = db.adminCommand("buildInfo");
var codeCoverageVariant = buildInfo.buildEnvironment.ccflags.includes("-ftest-coverage");
// If mongod was compiled with the code coverage flag, reduce some tests, as they take excessive
// time.

// test creating many collections to make sure no internal cache goes OOM
var loopNum = codeCoverageVariant ? 100 : 10000;
for (var i = 0; i < loopNum; ++i) {
    name = "memoryTest" + i;
    if ((i % 1000) == 0)
        print("Processing " + name);
    db.eval(function(col) {
        for (var i = 0; i < 100; ++i) {
            db[col + "_" + i].find();
        }
    }, name);
}

// test recovery of JS engine after out of memory
db.system.js.save({
    "_id": "f1",
    "value": function(n) {
        a = [];
        b = [];
        c = [];
        for (i = 0; i < n; i++) {
            a.push(Math.random());
            b.push(Math.random());
            c.push(Math.random());
        }
    }
});

// do mix of calls to make sure OOM is handled with no permanent damage
db.eval("f1(10)");
assert.throws(function() {
    db.eval("f1(100000000)");
});
db.eval("f1(10)");
assert.throws(function() {
    db.eval("f1(1000000000)");
});

loopNum = codeCoverageVariant ? 10000 : 1000000;
db.eval("f1(" + loopNum + ")");
db.eval("f1(" + loopNum + ")");
db.eval("f1(" + loopNum + ")");
assert.throws(function() {
    db.eval("f1(100000000)");
});
db.eval("f1(10)");
db.eval("f1(" + loopNum + ")");
db.eval("f1(" + loopNum + ")");
db.eval("f1(" + loopNum + ")");

// also test $where
col.drop();
col.insert({a: 1});
col.findOne({$where: "var arr = []; for (var i = 0; i < " + loopNum + "; ++i) {arr.push(0);}"});
assert.throws(function() {
    col.findOne({$where: "var arr = []; for (var i = 0; i < 1000000000; ++i) {arr.push(0);}"});
});
col.findOne({$where: "var arr = []; for (var i = 0; i < " + loopNum + "; ++i) {arr.push(0);}"});
