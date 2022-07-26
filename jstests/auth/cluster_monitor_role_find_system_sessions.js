// Verify that the clusterMonitor role can access config.system.sessions

(function() {
'use strict';

function runTestAs(conn, role) {
    const admin = conn.getDB('admin');
    const config = conn.getDB('config');

    assert(admin.auth('admin', 'admin'));
    const user = role + 'User';
    assert.commandWorked(admin.runCommand({createUser: user, pwd: 'pwd', roles: [role]}));
    admin.logout();

    assert(admin.auth(user, 'pwd'));
    jsTest.log('Acting as user with the ' + role + ' role');
    jsTest.log(assert.commandWorked(admin.runCommand({connectionStatus: 1, showPrivileges: 1})));

    assert.commandWorked(config.runCommand({collStats: 'system.sessions'}));
    assert.commandFailedWithCode(config.runCommand({collStats: 'system.version'}),
                                 ErrorCodes.Unauthorized);
    admin.logout();
}

function runTest(conn) {
    const admin = conn.getDB('admin');
    const config = conn.getDB('config');
    assert.commandWorked(
        admin.runCommand({createUser: 'admin', pwd: 'admin', roles: ['__system']}));

    runTestAs(conn, 'clusterMonitor');
}

const standalone = MongoRunner.runMongod({auth: ''});
runTest(standalone);
MongoRunner.stopMongod(standalone);
})();
