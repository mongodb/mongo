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
    for (var j = 0; j < 100; ++j) {
        db[name + "_" + j].find();
    }
}

// do mix of calls to make sure OOM is handled with no permanent damage
function doWhereTest(count) {
    'use strict';
    print('doWhereTest(' + count + ')');
    const coll = db.whereCol;
    coll.drop();
    coll.insert({a: 1});
    coll.findOne({$where: "var arr = []; for (var i = 0; i < " + count + "; ++i) {arr.push(0);}"});
}

doWhereTest(10);
assert.throws(function() {
    doWhereTest(1000000000);
});
doWhereTest(10);
assert.throws(function() {
    doWhereTest(1000000000);
});

loopNum = codeCoverageVariant ? 10000 : 1000000;
doWhereTest(loopNum);
doWhereTest(loopNum);
doWhereTest(loopNum);
assert.throws(function() {
    doWhereTest(1000000000);
});

doWhereTest(10);
doWhereTest(loopNum);
doWhereTest(loopNum);
doWhereTest(loopNum);
