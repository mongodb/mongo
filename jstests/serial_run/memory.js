// @tags: [
//   requires_fast_memory,
//   requires_scripting,
// ]
import {isMozjsWasm} from "jstests/libs/js_engine_util.js";

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "unable to start mongod");
const db = conn.getDB("memory_test");

const wasmEngine = isMozjsWasm(db);
if (wasmEngine) {
    // Cap WASM linear memory so the $where OOM tests are caught by the Wasmtime store
    // limiter (ExceededMemoryLimit) rather than the OS OOM killer. 100 MB JS heap +
    // 300 MB store leaves room for the 1M-element non-OOM workload while ensuring the
    // 1B-element allocation OOMs within WASM before physical RAM is exhausted.
    assert.commandWorked(db.adminCommand({setParameter: 1, jsHeapLimitMB: 100}));
    assert.commandWorked(db.adminCommand({setParameter: 1, wasmtimeStoreMemoryLimitMB: 300}));
}

let col = db.memoryTest;

let buildInfo = assert.commandWorked(db.adminCommand("buildInfo"));
let serverStatus = assert.commandWorked(db.adminCommand("serverStatus"));

// If mongod was compiled with the code coverage flag, then we reduce the length of some of the
// tests as they take an excessive amount of time. If the mongod is running with an in-memory
// storage engine, then we reduce the length of some of the tests to avoid an OOM due to the number
// of documents inserted.
let codeCoverageVariant = buildInfo.buildEnvironment.ccflags.includes("-ftest-coverage");
let inMemoryStorageEngine = !serverStatus.storageEngine.persistent;
let reduceNumLoops = codeCoverageVariant || inMemoryStorageEngine;

// test creating many collections to make sure no internal cache goes OOM
let loopNum = reduceNumLoops ? 100 : 10000;
for (let i = 0; i < loopNum; ++i) {
    const name = "memoryTest" + i;
    if (i % 1000 == 0) print("Processing " + name);
    for (let j = 0; j < 100; ++j) {
        db[name + "_" + j].find();
    }
}

// do mix of calls to make sure OOM is handled with no permanent damage
function doWhereTest(count) {
    print("doWhereTest(" + count + ")");
    const coll = db.whereCol;
    coll.drop();
    coll.insert({a: 1});
    coll.findOne({$where: "var arr = []; for (var i = 0; i < " + count + "; ++i) {arr.push(0);}"});
}

function assertMemoryError(func) {
    try {
        func();
    } catch (e) {
        if (
            e.message.includes("Out of memory") ||
            e.message.includes("JavaScript execution interrupted") ||
            e.message.includes("out of memory") ||
            e.message.includes("ExceededMemoryLimit") // WASM store limiter OOM
        ) {
            return;
        }
        throw e;
    }
    throw new Error("did not throw exception");
}

doWhereTest(10);
assertMemoryError(function () {
    doWhereTest(1000000000);
});
doWhereTest(10);
assertMemoryError(function () {
    doWhereTest(1000000000);
});

loopNum = reduceNumLoops ? 10000 : 1000000;
doWhereTest(loopNum);
doWhereTest(loopNum);
doWhereTest(loopNum);
assertMemoryError(function () {
    doWhereTest(1000000000);
});

doWhereTest(10);
doWhereTest(loopNum);
doWhereTest(loopNum);
doWhereTest(loopNum);

MongoRunner.stopMongod(conn);
