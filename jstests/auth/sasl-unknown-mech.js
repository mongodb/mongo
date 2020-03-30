// Test for saslStart invoked with invalid/missing mechanism.

(function() {
'use strict';

const mongod = MongoRunner.runMongod({auth: ''});
const admin = mongod.getDB('admin');

admin.createUser(
    {user: 'admin', pwd: 'pwd', roles: ['root'], mechanisms: ['SCRAM-SHA-1', 'SCRAM-SHA-256']});
admin.auth('admin', 'pwd');

// base64 encoded: 'n,,n=admin,r=deadbeefcafeba11';
const client1Payload = 'biwsbj1hZG1pbixyPWRlYWRiZWVmY2FmZWJhMTE=';

function saslStart(mechanism) {
    let cmd = {saslStart: 1};
    if (mechanism !== undefined) {
        cmd.mechanism = mechanism;
        cmd.payload = client1Payload;
    }
    jsTest.log(tojson(cmd));
    const result = admin.runCommand(cmd);
    printjson(result);
    return result;
}

function saslStartSuccess(mechanism) {
    const response = assert.commandWorked(saslStart(mechanism));
    assert.gt(response.payload.length, client1Payload.length);
    assert.eq(response.done, false);
    assert.gte(response.conversationId, 0);
}
saslStartSuccess('SCRAM-SHA-1');
saslStartSuccess('SCRAM-SHA-256');

function saslStartFailure(mechanism) {
    const response = assert.commandFailed(saslStart(mechanism));
    assert.gt(response.errmsg.length, 0);
    assert.neq(response.code, 0);
}
saslStartFailure('scram-sha-1');
saslStartFailure('MONGODB-CR');
saslStartFailure(undefined);

MongoRunner.stopMongod(mongod);
})();
