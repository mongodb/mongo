var col = db.memoryTest;

var buildInfo = assert.commandWorked(db.adminCommand("buildInfo"));
var serverStatus = assert.commandWorked(db.adminCommand("serverStatus"));

// If mongod was compiled with the code coverage flag, then we reduce the length of some of the
// tests as they take an excessive amount of time. If the mongod is running with an in-memory
// storage engine, then we reduce the length of some of the tests to avoid an OOM due to the number
// of documents inserted.
var codeCoverageVariant = buildInfo.buildEnvironment.ccflags.includes("-ftest-coverage");
var inMemoryStorageEngine = !serverStatus.storageEngine.persistent;
var reduceNumLoops = codeCoverageVariant || inMemoryStorageEngine;

// test creating many collections to make sure no internal cache goes OOM
var loopNum = reduceNumLoops ? 100 : 10000;
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

function assertMemoryError(func) {
    try {
        func();
    } catch (e) {
        if (e.message.includes('"errmsg" : "Out of memory"')) {
            return;
        }
        throw e;
    }
    throw new Error("did not throw exception");
}

doWhereTest(10);
assertMemoryError(function() {
    doWhereTest(1000000000);
});
doWhereTest(10);
assertMemoryError(function() {
    doWhereTest(1000000000);
});

loopNum = reduceNumLoops ? 10000 : 1000000;
doWhereTest(loopNum);
doWhereTest(loopNum);
doWhereTest(loopNum);
assertMemoryError(function() {
    doWhereTest(1000000000);
});

doWhereTest(10);
doWhereTest(loopNum);
doWhereTest(loopNum);
doWhereTest(loopNum);
