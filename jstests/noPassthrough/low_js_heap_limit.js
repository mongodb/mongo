// SERVER-26596 This tests that you can set a very low heap limit for javascript, and that it will
// fail to run any javascript, but won't crash the server.
(function() {
'use strict';

const conn = MongoRunner.runMongod();
var db = conn.getDB('db');

assert.commandWorked(db.adminCommand({setParameter: 1, jsHeapLimitMB: 1}));

db.foo.insert({x: 1});
const e = assert.throws(() => db.foo.findOne({$where: 'sleep(10000);'}));
assert.eq(e.code, ErrorCodes.ExceededMemoryLimit);

var returnCode = runProgram("mongo", "--jsHeapLimitMB=1", "--nodb", "--eval='exit();'");
assert.eq(returnCode, 1);
MongoRunner.stopMongod(conn);
}());
