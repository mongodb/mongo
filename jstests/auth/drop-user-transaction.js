// Validate dropUser performed via transaction.
// @tags: [requires_replication,exclude_from_large_txns]

(function() {
'use strict';

function runTest(conn, testCB) {
    const admin = conn.getDB('admin');
    const test = conn.getDB('test');
    admin.createUser({user: 'admin', pwd: 'pwd', roles: ['__system']});
    admin.auth('admin', 'pwd');

    // user1 -> role2 -> role1
    //      \___________.^
    assert.commandWorked(test.runCommand({createRole: 'role1', roles: [], privileges: []}));
    assert.commandWorked(test.runCommand({createRole: 'role2', roles: ['role1'], privileges: []}));
    assert.commandWorked(
        test.runCommand({createUser: 'user1', roles: ['role1', 'role2'], pwd: 'pwd'}));

    const beforeDrop = assert.commandWorked(test.runCommand({usersInfo: 'user1'})).users[0].roles;
    assert.eq(beforeDrop.length, 2);
    assert.eq(beforeDrop.map((r) => r.role).sort(), ['role1', 'role2']);

    testCB(test);

    // Callback should end up dropping role1
    // And we should have no references left to it.
    const allUsers = assert.commandWorked(test.runCommand({usersInfo: 1})).users;
    assert.eq(allUsers.length, 1);
    assert.eq(allUsers[0]._id, 'test.user1');
    assert.eq(allUsers[0].roles.map((r) => r.role), ['role2']);

    const allRoles = assert.commandWorked(test.runCommand({rolesInfo: 1})).roles;
    assert.eq(allRoles.length, 1);
    assert.eq(allRoles[0]._id, 'test.role2');
    assert.eq(allRoles[0].roles.length, 0);

    admin.logout();
}

//// Standalone
// We don't have transactions in standalone mode.
// Behavior elides transaction machinery, but is still protected by
// local mutex on the UMC commands.
// Expect the second command to block.
{
    const kFailpointDelay = 10 * 1000;
    const mongod = MongoRunner.runMongod({auth: null});
    assert.commandWorked(mongod.getDB('admin').runCommand({
        configureFailPoint: 'umcTransaction',
        mode: 'alwaysOn',
        data: {commitDelayMS: NumberInt(kFailpointDelay)},
    }));

    runTest(mongod, function(test) {
        // Pause and cause next op to block.
        const start = Date.now();
        const parallelShell = startParallelShell(
            `
            db.getSiblingDB('admin').auth('admin', 'pwd');
            assert.commandWorked(db.getSiblingDB('test').runCommand({dropRole: 'role1'}));
        `,
            mongod.port);

        // Other UMCs block.
        assert.commandWorked(test.runCommand({updateRole: 'role2', privileges: []}));
        parallelShell();
        assert.gte(Date.now() - start, kFailpointDelay);
    });

    MongoRunner.stopMongod(mongod);
}

//// ReplicaSet
// Ensure that dropRoles generates a transaction by checking for applyOps.
{
    const rst = new ReplSetTest({nodes: 3, keyFile: 'jstests/libs/key1'});
    rst.startSet();
    rst.initiate();
    rst.awaitSecondaryNodes();

    function relevantOp(op) {
        return ((op.op === 'u') || (op.op === 'd')) &&
            ((op.ns === 'admin.system.users') || (op.ns === 'admin.system.roles'));
    }

    function probableTransaction(op) {
        return (op.op === 'c') && (op.ns === 'admin.$cmd') && (op.o.applyOps !== undefined) &&
            op.o.applyOps.some(relevantOp);
    }

    runTest(rst.getPrimary(), function(test) {
        assert.commandWorked(test.runCommand({dropRole: 'role1'}));
        const oplog = test.getSiblingDB('local').oplog.rs.find({}).toArray();
        jsTest.log('Oplog: ' + tojson(oplog));

        // Events were not executed directly on the collections.
        const updatesAndDrops = oplog.filter(relevantOp);
        assert.eq(updatesAndDrops.length,
                  0,
                  'Found expected actions on priv collections: ' + tojson(updatesAndDrops));

        // They were executed by way of a transaction.
        const txns = oplog.filter(probableTransaction);
        assert.eq(
            txns.length, 1, 'Found unexpected number of probable transactions: ' + tojson(txns));

        const txnOps = txns[0].o.applyOps;
        assert.eq(
            txnOps.length, 3, 'Found unexpected number of ops in transaction: ' + tojson(txnOps));

        // Op1: Remove 'role1' from user1
        const msgUpdateUser = 'First op should be update admin.system.users' + tojson(txnOps);
        assert.eq(txnOps[0].op, 'u', msgUpdateUser);
        assert.eq(txnOps[0].ns, 'admin.system.users', msgUpdateUser);
        assert.eq(txnOps[0].o2._id, 'test.user1', msgUpdateUser);
        assert.eq(txnOps[0].o.diff.u.roles, [{role: 'role2', db: 'test'}], msgUpdateUser);

        // Op2: Remove 'role1' from role2
        const msgUpdateRole = 'Second op should be update admin.system.roles' + tojson(txnOps);
        assert.eq(txnOps[1].op, 'u', msgUpdateRole);
        assert.eq(txnOps[1].ns, 'admin.system.roles', msgUpdateRole);
        assert.eq(txnOps[1].o2._id, 'test.role2', msgUpdateRole);
        assert.eq(txnOps[1].o.diff.u.roles, [], msgUpdateRole);

        // Op3: Remove 'role1' document
        const msgDropRole = 'Third op should be drop from admin.system.roles' + tojson(txnOps);
        assert.eq(txnOps[2].op, 'd', msgDropRole);
        assert.eq(txnOps[2].ns, 'admin.system.roles', msgUpdateRole);
        assert.eq(txnOps[2].o._id, 'test.role1', msgUpdateRole);

        jsTest.log('Oplog applyOps: ' + tojson(txns));
    });

    rst.stopSet();
}
})();
