/**
 * Check that builtin roles contain valid permissions.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

(function() {
'use strict';

function assertIsBuiltinRole(def, name, expectPrivs = false, expectAuthRest = false) {
    jsTest.log(tojson(def));
    assert.eq(def.db, name.db);
    assert.eq(def.role, name.role);
    assert.eq(def.isBuiltin, true);
    assert.eq(def.roles.length, 0);
    assert.eq(def.inheritedRoles.length, 0);
    if (expectPrivs === false) {
        assert(def.privileges === undefined);
        assert(def.inheritedPrivileges === undefined);
    } else if (expectPrivs === true) {
        assert.gt(def.privileges.length, 0);
        assert.eq(bsonWoCompare(def.privileges, def.inheritedPrivileges), 0);
    } else {
        assert.eq(bsonWoCompare(def.privileges, expectPrivs), 0);
        assert.eq(bsonWoCompare(def.privileges, def.inheritedPrivileges), 0);
    }
    if (expectAuthRest === false) {
        assert(def.authenticationRestrictions === undefined);
        assert(def.inheritedAuthenticationRestrictions === undefined);
    } else if (expectAuthRest === true) {
        assert.eq(def.authenticationRestrictions.length, 0);
        assert.eq(def.inheritedAuthenticationRestrictions.length, 0);
    } else {
        // Builtin roles never have authentication restrictions.
        assert(false);
    }
}

function runTest(mongo) {
    const admin = mongo.getDB('admin');
    const test = mongo.getDB('test');

    function rolesInfo(db, role, basic = false) {
        const cmd = {
            rolesInfo: role,
            showPrivileges: !basic,
            showAuthenticationRestrictions: !basic,
            showBuiltinRoles: !basic,
        };
        return assert.commandWorked(db.runCommand(cmd))
            .roles.sort((a, b) => (a.role + '.' + a.db).localeCompare(b.role + '.' + b.db, 'en'));
    }

    const kTestReadRole = {role: 'read', db: 'test'};
    const kAdminReadRole = {role: 'read', db: 'admin'};
    const kReadRoleActions = [
        'changeStream',
        'collStats',
        'dbHash',
        'dbStats',
        'find',
        'killCursors',
        'listCollections',
        'listIndexes',
        'planCacheRead'
    ];
    const kAdminReadPrivs = [
        {resource: {db: 'admin', collection: ''}, actions: kReadRoleActions},
        {resource: {db: 'admin', collection: 'system.js'}, actions: kReadRoleActions},
    ];
    const kTestReadPrivs = [
        {resource: {db: 'test', collection: ''}, actions: kReadRoleActions},
        {resource: {db: 'test', collection: 'system.js'}, actions: kReadRoleActions},
    ];

    const adminReadBasic = rolesInfo(admin, 'read', true);
    assert.eq(adminReadBasic.length, 1);
    assertIsBuiltinRole(adminReadBasic[0], kAdminReadRole, false, false);

    const adminRead = rolesInfo(admin, 'read');
    assert.eq(adminRead.length, 1);
    assertIsBuiltinRole(adminRead[0], kAdminReadRole, true, true);
    assertIsBuiltinRole(adminRead[0], kAdminReadRole, kAdminReadPrivs, true);

    const testRead = rolesInfo(admin, kTestReadRole);
    assert.eq(testRead.length, 1);
    assertIsBuiltinRole(testRead[0], kTestReadRole, true, true);
    assertIsBuiltinRole(testRead[0], kTestReadRole, kTestReadPrivs, true);

    const testMulti = rolesInfo(admin, ['read', kTestReadRole]);
    assert.eq(testMulti.length, 2);
    assertIsBuiltinRole(testMulti[0], kAdminReadRole, kAdminReadPrivs, true);
    assertIsBuiltinRole(testMulti[1], kTestReadRole, kTestReadPrivs, true);

    const testRole1Privs = [{resource: {db: 'test', collection: ''}, actions: ['insert']}];
    assert.commandWorked(
        test.runCommand({createRole: 'role1', roles: ['read'], privileges: testRole1Privs}));

    const testRoles = rolesInfo(test, 1);
    const testUserRoles = testRoles.filter((r) => !r.isBuiltin);
    assert.eq(testUserRoles.length, 1);
    const testUserRole1 = testUserRoles[0];
    jsTest.log('testUserRole1: ' + tojson(testUserRole1));
    assert.eq(testUserRole1.db, 'test');
    assert.eq(testUserRole1.role, 'role1');
    assert.eq(testUserRole1.roles, [kTestReadRole]);
    assert.eq(testUserRole1.inheritedRoles, [kTestReadRole]);
    assert.eq(testUserRole1.privileges, testRole1Privs);
    assert.eq(testUserRole1.inheritedPrivileges, testRole1Privs.concat(kTestReadPrivs));

    const testRolesReadRole = testRoles.filter((r) => r.role === 'read');
    assert.eq(testRolesReadRole.length, 1);
    assertIsBuiltinRole(testRolesReadRole[0], kTestReadRole, kTestReadPrivs, true);
}

// Standalone
const mongod = MongoRunner.runMongod();
runTest(mongod);
MongoRunner.stopMongod(mongod);

const st = new ShardingTest({shards: 1, mongos: 1, config: 1});
runTest(st.s0);
st.stop();
})();
