// Tests the connectionStatus command
(function() {
    var dbName = 'connection_status';
    var myDB = db.getSiblingDB(dbName);
    myDB.dropAllUsers();

    function test(userName) {
        myDB.createUser(
            {user: userName, pwd: "weak password", roles: [{db: "admin", role: "root"}]});
        myDB.auth(userName, "weak password");

        var output = myDB.runCommand("connectionStatus");
        assert.commandWorked(output);

        // Test that authenticated users are properly returned.
        var users = output.authInfo.authenticatedUsers;

        var matches = 0;
        for (var i = 0; i < users.length; i++) {
            if (users[i].db != dbName)
                continue;

            assert.eq(users[i].user, userName);
            matches++;
        }
        assert.eq(matches, 1);

        // Test that authenticated roles are properly returned.
        var roles = output.authInfo.authenticatedUserRoles;

        matches = 0;
        for (var i = 0; i < roles.length; i++) {
            if (roles[i].db != "admin")
                continue;

            assert.eq(roles[i].role, "root");
            matches++;
        }
        assert(matches >= 1);

        // Test roles/ privileges for a non-root user.
        myDB.createUser({user: "foo", pwd: "weak password", roles: [{db: "foo", role: "read"}]});
        myDB.logout();
        myDB.auth("foo", "weak password");

        output = myDB.runCommand({"connectionStatus": 1, "showPrivileges": 1});
        assert.commandWorked(output);

        var users = output.authInfo.authenticatedUsers;
        var authedAsSystem = false;
        for (var i = 0; i < users.length; i++) {
            var authed = users[i];
            if (authed.user === "__system" && authed.db === "local") {
                authedAsSystem = true;
            }
        }

        var privileges = output.authInfo.authenticatedUserPrivileges;

        for (var i = 0; i < privileges.length; i++) {
            if (privileges[i].resource.anyResource) {
                if (authedAsSystem) {
                    assert.eq(["anyAction"],
                              privileges[i].actions,
                              "__system user should only have anyResource/anyAction privilege:" +
                                  tojson(output));
                } else {
                    assert(false,
                           "read role should not have anyResource privileges:" + tojson(output));
                }
            }
        }

        myDB.logout();

        // Clean up.
        myDB.auth(userName, "weak password");
        myDB.dropAllUsers();
        myDB.logout();
    }

    test("someone");
    test("someone else");
})();
