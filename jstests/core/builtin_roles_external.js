/**
 * Attempting to enumerate roles on the $external database should return an empty set.
 * @tags: [multiversion_incompatible,tenant_migration_incompatible]
 */
(function() {
"use strict";

function assertBuiltinRoles(dbname, shouldHaveRoles) {
    const allRoles = assert
                         .commandWorked(db.getSiblingDB(dbname).runCommand(
                             {rolesInfo: 1, showBuiltinRoles: 1, showPrivileges: 1}))
                         .roles;
    jsTest.log(dbname + ' roles: ' + tojson(allRoles));

    const builtinRoles = allRoles.filter((r) => r.isBuiltin);
    if (shouldHaveRoles) {
        assert.gt(builtinRoles.length, 0, dbname + ' should have builtin roles, but none returned');

        function assertRole(role, expect = true) {
            const filtered = builtinRoles.filter((r) => r.role === role);
            if (expect) {
                assert.gt(
                    filtered.length, 0, dbname + ' should have role ' + role + ' but does not');
            } else {
                assert.eq(
                    filtered.length,
                    0,
                    dbname + ' should have not role ' + role + ' but does: ' + tojson(filtered));
            }
        }

        assertRole('read');
        assertRole('readWrite');
        assertRole('readWriteAnyDatabase', dbname === 'admin');
        assertRole('hostManager', dbname === 'admin');
    } else {
        assert.eq(builtinRoles.length,
                  0,
                  dbname + ' should not have builtin roles, found: ' + tojson(builtinRoles));
    }
}

assertBuiltinRoles('admin', true);
assertBuiltinRoles('test', true);
assertBuiltinRoles('$external', false);
assertBuiltinRoles('$test', true);
}());
