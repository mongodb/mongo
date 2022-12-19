(function() {
'use strict';

load('jstests/libs/parallel_shell_helpers.js');

const mongod = MongoRunner.runMongod({auth: ''});
mongod.getDB('admin').createUser(
    {user: 'admin', pwd: 'pwd', roles: ['root'], mechanisms: ['SCRAM-SHA-1']});
assert(mongod.getDB('admin').auth('admin', 'pwd'));
// `mongod` is authenticated as a super user and stays that way.

// base64 encoded: 'n,,n=admin,r=deadbeefcafeba11';
const kClientPayload = 'biwsbj1hZG1pbixyPWRlYWRiZWVmY2FmZWJhMTE=';

const kFailedToAuthMsg = 5286307;

// Obtains all of the logs that contain a failed to authenticate message.
function getFailures() {
    // No need to auth, we're already superuser.
    return checkLog.getGlobalLog(mongod)
        .map(JSON.parse)
        .filter((log) => log.id == kFailedToAuthMsg);
}

// Ensures that there are exactly expectedNumFailures failures in the logs after failuresBefore.
function assertNewAuthFailures(failuresBefore, expectedNumFailures) {
    const failuresAfter = getFailures();
    assert.eq(failuresBefore.length + expectedNumFailures,
              failuresAfter.length,
              "Unexpected new failures: " + tojson(failuresAfter.slice(failuresBefore.length)));
}

function runTest(speculations, performNormalAuth = false) {
    const failuresBefore = getFailures();

    // Running the operations in a parallel shell so that if performNormalAuth is false, we
    // encounter an "authentication session abandoned" error because the client will disconnect.
    let runAuths = startParallelShell(
        funWithArgs(function(speculations, performNormalAuth = false) {
            const admin = db.getSiblingDB('admin');
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

    // If we perform a normal auth after the speculative one(s), we complete the authentication
    // session (and thus, do not encounter the "authentication session abandoned" error).
    let expectedFailures = 0;
    if (speculations > 0) {
        expectedFailures = performNormalAuth ? 0 : 1;
    }
    if (speculations > 1) {
        expectedFailures += speculations - 1;
    }

    assertNewAuthFailures(failuresBefore, expectedFailures);
}

// Run test with no speculation (0), normal speculation (1), and a few levels of excessive
// speculation. Try each degree both with and without performing a normal authentication at the end.
for (let speculate = 0; speculate < 5; ++speculate) {
    runTest(speculate, true);
    runTest(speculate, false);
}

MongoRunner.stopMongod(mongod);
})();
