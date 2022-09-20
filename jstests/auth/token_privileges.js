// Test role restrictions when using security tokens.
// @tags: [requires_replication]

(function() {
'use strict';

const tenantID = ObjectId();
const isMongoStoreEnabled = TestData.setParameters.featureFlagMongoStore;

if (!isMongoStoreEnabled) {
    assert.throws(() => MongoRunner.runMongod({
        setParameter: "multitenancySupport=true",
    }));
    return;
}

function runTest(conn, rst = undefined) {
    const admin = conn.getDB('admin');
    const external = conn.getDB('$external');

    // Must be authenticated as a user with ActionType::useTenant in order to use $tenant
    assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(admin.auth('admin', 'pwd'));

    // Create tenant-specific users.
    const users = {
        readOnlyUser: {roles: [{role: 'readAnyDatabase', db: 'admin'}]},
        readWriteUser: {roles: [{role: 'readWriteAnyDatabase', db: 'admin'}]},
        clusterAdminUser: {roles: [{role: 'clusterAdmin', db: 'admin'}], prohibited: true},
    };
    Object.keys(users).forEach(
        (user) => assert.commandWorked(external.runCommand(
            {createUser: user, '$tenant': tenantID, roles: users[user].roles})));
    if (rst) {
        rst.awaitReplication();
    }

    Object.keys(users).forEach(function(user) {
        const tokenConn = new Mongo(conn.host);
        tokenConn._setSecurityToken(
            _createSecurityToken({user: user, db: '$external', tenant: tenantID}));
        const tokenDB = tokenConn.getDB('test');
        if (users[user].prohibited) {
            assert.commandFailed(tokenDB.adminCommand({connectionStatus: 1}));
        } else {
            const authInfo =
                assert.commandWorked(tokenDB.adminCommand({connectionStatus: 1})).authInfo;
            jsTest.log(authInfo);

            assert.eq(authInfo.authenticatedUsers.length, 1);
            assert.eq(authInfo.authenticatedUsers[0].user, user);
            assert.eq(authInfo.authenticatedUsers[0].db, '$external');

            const authedRoles =
                authInfo.authenticatedUserRoles.map((role) => role.db + '.' + role.role);
            const expectRoles = users[user].roles.map((role) => role.db + '.' + role.role);
            const unexpectedRoles = authedRoles.filter((role) => !expectRoles.includes(role));
            assert.eq(unexpectedRoles.length, 0, 'Unexpected roles: ' + tojson(unexpectedRoles));
            const missingRoles = expectRoles.filter((role) => !authedRoles.includes(role));
            assert.eq(missingRoles.length, 0, 'Missing roles: ' + tojson(missingRoles));
        }
    });
}

const opts = {
    auth: '',
    setParameter: "multitenancySupport=true",
};
{
    const standalone = MongoRunner.runMongod(opts);
    assert(standalone !== null, "MongoD failed to start");
    runTest(standalone);
    MongoRunner.stopMongod(standalone);
}

// TODO SERVER-62395: Uncomment this test once we use tid to construct namespace when applying
// non-txn commands.
/* {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: opts});
    rst.startSet({keyFile: 'jstests/libs/key1'});
    rst.initiate();
    runTest(rst.getPrimary(), rst);
    rst.stopSet();
} */
})();
