// Test for auth counters in serverStatus.

const kFailedToAuthMsgId = 5286307;

const mongod = MongoRunner.runMongod();

try {
    mongod.getDB("admin").createUser(
        {"user": "admin", "pwd": "pwd", roles: ['root'], mechanisms: ["SCRAM-SHA-256"]});

    const shellCmd = () => {
        // base64 encoded: 'n,,n=admin,r=deadbeefcafeba11';
        const kClientPayload = 'biwsbj1hZG1pbixyPWRlYWRiZWVmY2FmZWJhMTE=';

        db.getSiblingDB("admin").runCommand(
            {saslStart: 1, mechanism: "SCRAM-SHA-256", payload: kClientPayload});
    };

    startParallelShell(shellCmd, mongod.port)();

    assert.soon(() => checkLog.checkContainsOnceJson(
                    mongod, kFailedToAuthMsgId, {"result": ErrorCodes.AuthenticationAbandoned}));

} finally {
    MongoRunner.stopMongod(mongod);
}