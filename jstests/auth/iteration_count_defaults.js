// Test SCRAM iterationCount defaults.

(function() {
'use strict';

function runOpt(params, sha1Value, sha256Value) {
    const conn = MongoRunner.runMongod({auth: '', setParameter: params});
    const adminDB = conn.getDB('admin');

    adminDB.createUser({user: 'user1', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(adminDB.auth({user: 'user1', pwd: 'pass'}));

    const response = assert.commandWorked(adminDB.runCommand(
        {getParameter: 1, scramIterationCount: 1, scramSHA256IterationCount: 1}));
    assert.eq(response.scramIterationCount, sha1Value);
    assert.eq(response.scramSHA256IterationCount, sha256Value);

    MongoRunner.stopMongod(conn);
}

runOpt({}, 10000, 15000);
runOpt({scramIterationCount: 12500}, 12500, 15000);
runOpt({scramIterationCount: 20000}, 20000, 20000);
runOpt({scramSHA256IterationCount: 9999}, 10000, 9999);
runOpt({scramSHA256IterationCount: 10001}, 10000, 10001);
runOpt({scramIterationCount: 7000, scramSHA256IterationCount: 8000}, 7000, 8000);
runOpt({scramIterationCount: 8000, scramSHA256IterationCount: 7000}, 8000, 7000);
})();
