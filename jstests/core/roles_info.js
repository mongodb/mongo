// Test invocations of {rolesInfo: ...} command
// @tags: [requires_multi_updates, requires_non_retryable_commands,requires_fcv_47]

(function() {
'use strict';

// Setup some sample roles.
const dbname = db.getName();
const fqReadRoleName = {
    role: 'read',
    db: dbname
};
const fqTestRoleName = {
    role: 'testRoleJSCoreRolesInfo',
    db: dbname
};
const testRolePrivs = [
    {resource: {db: dbname, collection: ''}, actions: ['insert']},
];
const testRoleRoles = [fqReadRoleName];
assert.commandWorked(db.runCommand(
    {createRole: 'testRoleJSCoreRolesInfo', privileges: testRolePrivs, roles: testRoleRoles}));

function matchRoleFn(match) {
    return (role) => (role.db === match.db) && (role.role === match.role);
}

function requireRole(roleSet, role) {
    const ret = roleSet.filter(matchRoleFn(role));
    assert.eq(ret.length, 1, "Didn't find " + tojson(role) + " in " + tojson(roleSet));
    return ret[0];
}

function requireNoRole(roleSet, role) {
    const ret = roleSet.filter(matchRoleFn(role));
    assert.eq(ret.length, 0, "Unexpectedly found " + tojson(role) + " in " + tojson(roleSet));
}

function checkForUserDefinedRole(roleSet, expectPrivs) {
    const role = requireRole(roleSet, fqTestRoleName);
    if (expectPrivs) {
        assert(bsonWoCompare(role.privileges, testRolePrivs) === 0,
               'Unexpected privileges in: ' + tojson(role));
    } else {
        assert(role.privileges === undefined, 'Unexpected privileges in: ' + tojson(role));
    }
    assert(bsonWoCompare(role.roles, testRoleRoles) === 0, 'Unexpected roles in: ' + tojson(role));
    assert(role.isBuiltin !== true, 'Unexpected isBuiltin: ' + tojson(role));
}

function checkForBuiltinRole(roleSet, fqRoleName) {
    const role = requireRole(roleSet, fqRoleName);
    assert.eq(role.roles.length, 0, 'Builtin roles must not have subordinates: ' + tojson(role));
    assert(role.isBuiltin !== false, 'Unexpected isBuiltin: ' + tojson(role));
}

function rolesInfo(query, showExtra = {}) {
    const cmd = Object.assign({rolesInfo: query}, showExtra);
    jsTest.log(tojson(cmd));
    const ret = assert.commandWorked(db.runCommand(cmd));
    assert(ret.roles !== undefined, 'No roles property in response');
    printjson(ret.roles);
    return ret.roles;
}

function rolesInfoSingle(query, extra = {}) {
    const ret = rolesInfo(query, extra);
    assert.eq(ret.length, 1, 'Unexpected roles (or no roles) in query: ' + tojson(query));
    return ret;
}

// {rolesInfo: 1, showBuiltinRoles: true, showPrivileges: true}
const allRoles = rolesInfo(1, {showBuiltinRoles: true, showPrivileges: true});
allRoles.forEach((r) => assert(r.isBuiltin !== undefined),
                 "Role must have 'isBuiltin' property for DB query");
checkForUserDefinedRole(allRoles, true);
checkForBuiltinRole(allRoles, fqReadRoleName);
requireNoRole(allRoles, {role: 'doesNotExist', db: dbname});

// {rolesInfo: 'testRoleJSCoreRolesInfo'} // User defined role
const stringRoles = rolesInfoSingle('testRoleJSCoreRolesInfo');
checkForUserDefinedRole(stringRoles, false);
requireNoRole(stringRoles, fqReadRoleName);

// {rolesInfo: 'read'} // Builtin role
const builtinStringRoles = rolesInfoSingle('read');
checkForBuiltinRole(builtinStringRoles, fqReadRoleName);

// {rolesInfo: {db: dbname, role: 'testRoleJSCoreRolesInfo'} // User defined role
const docRoles = rolesInfoSingle(fqTestRoleName);
checkForUserDefinedRole(docRoles, false);
requireNoRole(docRoles, fqReadRoleName);

// {rolesInfo: {db: dbname, role: 'read'} // Builtin role
const builtinDocRoles = rolesInfoSingle(fqReadRoleName);
checkForBuiltinRole(builtinDocRoles, fqReadRoleName);

// Multiroles: [ 'testRoleJSCoreRolesInfo', {db: dbname, role: 'read'}, 'readWrite' ]
const multiRoles = rolesInfo(['testRoleJSCoreRolesInfo', fqReadRoleName, 'readWrite']);
assert.eq(multiRoles.length, 3, 'Incorrect number of roles returned: ' + tojson(multiRoles));
checkForUserDefinedRole(multiRoles, false);
checkForBuiltinRole(multiRoles, fqReadRoleName);
checkForBuiltinRole(multiRoles, {db: dbname, role: 'readWrite'});

assert.commandWorked(db.runCommand({dropRole: 'testRoleJSCoreRolesInfo'}));
})();
