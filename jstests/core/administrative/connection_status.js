// Tests the connectionStatus command
//
// @tags: [
//   # The test runs commands that are not allowed with security token: ConnectionStatus,
//   # connectionStatus, createUser, logout.
//   not_allowed_with_signed_security_token,
//   assumes_superuser_permissions,
//   assumes_write_concern_unchanged,
//   creates_and_authenticates_user,
//   requires_auth,
//   requires_non_retryable_commands,
// ]

const kAdminDbName = "admin";
const kTestDbName = "connection_status";

const myDB = db.getSiblingDB(kTestDbName);

myDB.dropAllUsers();
db.logout(); // logout from the current db - anecodtally "test_autocomplete" - to avoid double
// authn errors

/**
 * Test that the output of connectionStatus makes sense.
 */
function validateConnectionStatus(expectedUser, expectedRole, showPrivileges) {
    let connectionStatus = myDB.runCommand({"connectionStatus": 1, "showPrivileges": showPrivileges});
    assert.commandWorked(connectionStatus);
    let authInfo = connectionStatus.authInfo;

    // Test that UUID is properly returned.
    // This UUID is from the runCommand connection, not the user, so it cannot be asserted against
    // the userId.
    const uuid = connectionStatus.uuid;
    assert(uuid, "UUID returned from runCommand is falsy: " + tojson(uuid));

    const parsedUUID = JSON.parse(JSON.stringify(uuid));
    const kUUIDSubtype = 4;
    assert.eq(NumberInt(parsedUUID["$type"]), kUUIDSubtype, "UUID field should be a BinDataUUID, got: " + tojson(uuid));
    assert(parsedUUID["$binary"], "Missing payload for client UUID: " + tojson(uuid));

    // Test that authenticated users are properly returned.
    let users = authInfo.authenticatedUsers;
    let matches = 0;
    let infoStr = tojson(authInfo);
    for (var i = 0; i < users.length; i++) {
        let user = users[i].user;
        var db = users[i].db;
        assert(isString(user), "each authenticatedUsers should have a 'user' string:" + infoStr);
        assert(isString(db), "each authenticatedUsers should have a 'db' string:" + infoStr);
        if (user === expectedUser.user && db === expectedUser.db) {
            matches++;
        }
    }
    assert.eq(matches, 1, "expected user should be present once in authenticatedUsers:" + infoStr);

    // Test that authenticated roles are properly returned.
    let roles = authInfo.authenticatedUserRoles;
    matches = 0;
    for (var i = 0; i < roles.length; i++) {
        let role = roles[i].role;
        var db = roles[i].db;
        assert(isString(role), "each authenticatedUserRole should have a 'role' string:" + infoStr);
        assert(isString(db), "each authenticatedUserRole should have a 'db' string:" + infoStr);
        if (role === expectedRole.role && db === expectedRole.db) {
            matches++;
        }
    }
    // Role will be duplicated when users with the same role are logged in at the same time.
    assert.gte(matches, 1, "expected role should be present in authenticatedUserRoles:" + infoStr);

    let privileges = authInfo.authenticatedUserPrivileges;
    if (showPrivileges) {
        for (var i = 0; i < privileges.length; i++) {
            assert(
                isObject(privileges[i].resource),
                "each authenticatedUserPrivilege should have a 'resource' object:" + infoStr,
            );
            let actions = privileges[i].actions;
            for (let j = 0; j < actions.length; j++) {
                assert(isString(actions[j]), "each authenticatedUserPrivilege action should be a string:" + infoStr);
            }
        }
    } else {
        // Test that privileges are not returned without asking
        assert.eq(privileges, undefined, "authenticatedUserPrivileges should not be returned by default:" + infoStr);
    }
}

function test(userName) {
    const user = {user: userName, db: kTestDbName};
    const role = {role: "root", db: kAdminDbName};
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
