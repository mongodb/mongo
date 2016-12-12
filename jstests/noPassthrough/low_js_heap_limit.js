// SERVER-26596 This tests that you can set a very low heap limit for javascript, and that it will
// fail to run any javascript, but won't crash the server.
(function() {
    'use strict';

    const conn = MongoRunner.runMongod();
    var db = conn.getDB('db');

    assert.commandWorked(db.adminCommand({setParameter: 1, jsHeapLimitMB: 1}));

    assert.commandFailedWithCode(db.runCommand({$eval: 'sleep(10000);'}),
                                 ErrorCodes.ExceededMemoryLimit);

    var returnCode = runProgram("mongo", "--jsHeapLimitMB=1", "--nodb", "--eval='exit();'");
    assert.eq(returnCode, 1);
}());
