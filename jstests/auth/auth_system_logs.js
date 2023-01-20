(function() {
'use strict';

load('jstests/libs/parallel_shell_helpers.js');

const mongod = MongoRunner.runMongod({auth: ''});
mongod.getDB('admin').createUser(
    {user: 'admin', pwd: 'pwd', roles: ['root'], mechanisms: ['SCRAM-SHA-1']});
assert(mongod.getDB('admin').auth('admin', 'pwd'));
// `mongod` is authenticated as a super user and stays that way.

const kFailedToAuthMsg = 5286307;

let failuresAlreadyObserved = 0;

// Run the number of speculations specified, and if performNormalAuth is true we complete the
// authentication session. At the end, we check to see that the total number of failures in the logs
// for this iteration is the same as expectedNumFailures.
function runTest(speculations, performNormalAuth, expectedNumFailures) {
    // Running the operations in a parallel shell so that if performNormalAuth is false, we
    // encounter an "authentication session abandoned" error because the client will disconnect.
    let runAuths = startParallelShell(
        funWithArgs(function(speculations, performNormalAuth = false) {
            const admin = db.getSiblingDB('admin');

            // base64 encoded: 'n,,n=admin,r=deadbeefcafeba11';
            const kClientPayload = 'biwsbj1hZG1pbixyPWRlYWRiZWVmY2FmZWJhMTE=';

            // Run speculative auth(s).
            for (let i = 0; i < speculations; ++i) {
                assert.commandWorked(admin.runCommand({
                    hello: 1,
                    speculativeAuthenticate: {
                        saslStart: 1,
                        mechanism: "SCRAM-SHA-1",
                        payload: kClientPayload,
                        db: "admin"
                    }
                }));
            }

            if (performNormalAuth) {
                assert(admin.auth({user: 'admin', pwd: 'pwd', mechanism: 'SCRAM-SHA-1'}));
            }
        }, speculations, performNormalAuth), mongod.port);

    runAuths();

    checkLog.containsWithCount(
        mongod, kFailedToAuthMsg, failuresAlreadyObserved + expectedNumFailures);
    failuresAlreadyObserved += expectedNumFailures;
}

// Running with 0 speculations should not cause any failures.
runTest(0, true, 0);
runTest(0, false, 0);

// Running with 1 speculation should only cause a failure when we do not complete the authentication
// session with a normal auth.
runTest(1, true, 0);
runTest(1, false, 1);

// Running 2 speculations should cause a failure because the second will override the first.
runTest(2, true, 1);
runTest(2, false, 2);

MongoRunner.stopMongod(mongod);
})();
