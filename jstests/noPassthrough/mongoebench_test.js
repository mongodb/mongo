/**
 * Tests for the mongoebench executable.
 */
(function() {
"use strict";

load("jstests/libs/mongoebench.js");  // for runMongoeBench

if (jsTest.options().storageEngine !== "mobile") {
    print("Skipping test because storage engine isn't mobile");
    return;
}

const dbpath = MongoRunner.dataPath + "mongoebench_test";
resetDbpath(dbpath);

// Test that the operations in the "pre" section of the configuration are run exactly once.
runMongoeBench(  // Force clang-format to break this line.
    {
        pre: [{
            op: "insert",
            ns: "test.mongoebench_test",
            doc: {pre: {"#SEQ_INT": {seq_id: 0, start: 0, step: 1, unique: true}}}
        }],
        ops: [{
            op: "update",
            ns: "test.mongoebench_test",
            update: {$inc: {ops: 1}},
            multi: true,
        }]
    },
    {dbpath});

const output = cat(dbpath + "/perf.json");
const stats = assert.doesNotThrow(
    JSON.parse, [output], "failed to parse output file as strict JSON: " + output);
assert.eq({$numberLong: "0"},
          stats.errCount,
          () => "stats file reports errors but exit code was zero: " + tojson(stats));
assert(stats.hasOwnProperty("totalOps/s"),
       () => "stats file doesn't report ops per second: " + tojson(stats));

const conn = MongoRunner.runMongod({dbpath, noCleanData: true});
assert.neq(null, conn, "failed to start mongod after running mongoebench");

const db = conn.getDB("test");
const count = db.mongoebench_test.find().itcount();
assert.eq(1, count, "ops in 'pre' section ran more than once or didn't run at all");

MongoRunner.stopMongod(conn);
})();
