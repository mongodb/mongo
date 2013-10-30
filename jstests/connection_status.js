// Tests the connectionStatus command

var dbName = 'connection_status';
var myDB = db.getSiblingDB(dbName);
myDB.dropAllUsers();

function test(userName) {
    myDB.addUser({user: userName, pwd: "weak password", roles: jsTest.basicUserRoles});
    myDB.auth(userName, "weak password");

    var output = myDB.runCommand("connectionStatus");
    assert.commandWorked(output);
    var users = output.authInfo.authenticatedUsers;

    var matches = 0;
    for (var i=0; i < users.length; i++) {
        if (users[i].db != dbName)
            continue;

        assert.eq(users[i].user, userName);
        matches++;
    }
    assert.eq(matches, 1);
}

test("someone");
test("someone else"); // replaces someone
