// Ensure that benchRun tests are able to use either SCRAM-SHA-1 or SCRAM-SHA-256 via mech
// negotiation from server
(function() {
"use strict";

function benchRunnerAuthWithProvidedMech(mechanism) {
    var m = MongoRunner.runMongod({setParameter: 'authenticationMechanisms=' + mechanism});

    const db = 'admin';
    const user = 'scram_test';
    const pwd = 'something';

    const admin = m.getDB(db);
    admin.createUser({user: user, pwd: pwd, roles: [], mechanisms: [mechanism]});

    const ops = [];

    const seconds = 1;

    const benchArgs = {
        ops: ops,
        parallel: 2,
        seconds: seconds,
        host: m.host,
        db: db,
        username: user,
        password: pwd
    };

    const res = assert.doesNotThrow(
        benchRun, [benchArgs], "BenchRun attempted SASL negotiation. Server supports " + mechanism);

    printjson(res);

    MongoRunner.stopMongod(m);
}

benchRunnerAuthWithProvidedMech("SCRAM-SHA-1");
benchRunnerAuthWithProvidedMech("SCRAM-SHA-256");
})();
