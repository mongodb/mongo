'use strict';

const kInvalidationIntervalSecs = 5;

// Test that sessions cannot be resumed by deleted and recreated user.
function runTest(s0, s1) {
    assert(s0);
    assert(s1);
    const admin = s0.getDB('admin');

    function checkIdType(username) {
        const user = admin.system.users.find({user: username, db: 'admin'}).toArray()[0];
        const id = user._id;
        const userId = user.userId;
        assert.eq(typeof (id), 'string');
        assert.eq(id, 'admin.' + username);
        assert.eq(typeof (userId), 'object');
        assert.eq(tojson(userId).substring(0, 5), 'UUID(');
    }

    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(admin.auth('admin', 'pass'));
    checkIdType('admin');

    admin.createUser({user: 'user', pwd: 'pass', roles: jsTest.basicUserRoles});
    checkIdType('user');
    admin.logout();

    // Connect as basic user and create a session.
    assert(admin.auth('user', 'pass'));
    assert.commandWorked(admin.mycoll.insert({_id: "foo", data: "bar"}));

    // Perform administrative commands via separate shell.
    function evalCmd(cmd) {
        const uri = 'mongodb://admin:pass@localhost:' + s1.port + '/admin';
        const result = runMongoProgram('./mongo', uri, '--eval', cmd);
        assert.eq(result, 0, "Command failed");
    }
    evalCmd('db.dropUser("user"); ');
    evalCmd('db.createUser({user: "user", pwd: "secret", roles: ["root"]});');

    if (s0 !== s1) {
        // Wait for twice the invalidation interval when sharding.
        sleep(2 * kInvalidationIntervalSecs * 1000);
    }

    // This should fail due to invalid user session.
    const thrown =
        assert.throws(() => admin.mycoll.find({}).toArray(), [], "Able to find after recreate");
    assert.eq(thrown.code, ErrorCodes.Unauthorized, "Threw something other than unauthorized");
}
