// Tests the connectionStatus command
(function() {
    "use strict";
    var dbName = 'connection_status';
    var myDB = db.getSiblingDB(dbName);
    myDB.dropAllUsers();

    /**
     * Test that the output of connectionStatus makes sense.
     */
    function validateConnectionStatus(expectedUser, expectedRole, showPrivileges) {
        var connectionStatus =
            myDB.runCommand({"connectionStatus": 1, "showPrivileges": showPrivileges});
        assert.commandWorked(connectionStatus);
        var authInfo = connectionStatus.authInfo;

        // Test that authenticated users are properly returned.
        var users = authInfo.authenticatedUsers;
        var matches = 0;
        var infoStr = tojson(authInfo);
        for (var i = 0; i < users.length; i++) {
            var user = users[i].user;
            var db = users[i].db;
            assert(isString(user),
                   "each authenticatedUsers should have a 'user' string:" + infoStr);
            assert(isString(db), "each authenticatedUsers should have a 'db' string:" + infoStr);
            if (user === expectedUser.user && db === expectedUser.db) {
                matches++;
            }
        }
        assert.eq(
            matches, 1, "expected user should be present once in authenticatedUsers:" + infoStr);

        // Test that authenticated roles are properly returned.
        var roles = authInfo.authenticatedUserRoles;
        matches = 0;
        for (var i = 0; i < roles.length; i++) {
            var role = roles[i].role;
            var db = roles[i].db;
            assert(isString(role),
                   "each authenticatedUserRole should have a 'role' string:" + infoStr);
            assert(isString(db), "each authenticatedUserRole should have a 'db' string:" + infoStr);
            if (role === expectedRole.role && db === expectedRole.db) {
                matches++;
            }
        }
        // Role will be duplicated when users with the same role are logged in at the same time.
        assert.gte(
            matches, 1, "expected role should be present in authenticatedUserRoles:" + infoStr);

        var privileges = authInfo.authenticatedUserPrivileges;
        if (showPrivileges) {
            for (var i = 0; i < privileges.length; i++) {
                assert(
                    isObject(privileges[i].resource),
                    "each authenticatedUserPrivilege should have a 'resource' object:" + infoStr);
                var actions = privileges[i].actions;
                for (var j = 0; j < actions.length; j++) {
                    assert(isString(actions[j]),
                           "each authenticatedUserPrivilege action should be a string:" + infoStr);
                }
            }

        } else {
            // Test that privileges are not returned without asking
            assert.eq(privileges,
                      undefined,
                      "authenticatedUserPrivileges should not be returned by default:" + infoStr);
        }
    }

    function test(userName) {
        var user = {user: userName, db: dbName};
        var role = {role: "root", db: "admin"};
        myDB.createUser({user: userName, pwd: "weak password", roles: [role]});
        myDB.auth(userName, "weak password");

        // Validate with and without showPrivileges
        validateConnectionStatus(user, role, true);
        validateConnectionStatus(user, role, false);

        // Clean up.
        myDB.dropAllUsers();
        myDB.logout();
    }

    test("someone");
    test("someone else");
})();
