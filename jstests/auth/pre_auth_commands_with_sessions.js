let conn = MongoRunner.runMongod({auth: ""});
let admin = conn.getDB("admin");
var db = conn.getDB("otherdb");

admin.createUser({user: "admin", pwd: "pwd", roles: jsTest.adminUserRoles});
admin.auth("admin", "pwd");
db.createUser({user: "lily", pwd: "pwd", roles: jsTest.basicUserRoles});
admin.logout();

let testCommand = function (cmd) {
    // Test that we can run a pre-auth command without authenticating.
    let command = {[cmd]: 1};

    assert.commandWorked(admin.runCommand(command));

    // Test that we can authenticate and start a session
    db.auth("lily", "pwd");
    let res = admin.runCommand({startSession: 1});
    assert.commandWorked(res);

    let commandWithSession = {[cmd]: 1, lsid: res.id};

    // Test that we can run a pre-auth command with a session while
    // the session owner is logged in (and the session gets ignored)
    assert.commandWorked(db.runCommand(command), "failed to run command " + cmd + " while logged in");
    assert.commandWorked(
        db.runCommand(commandWithSession),
        "failed to run command " + cmd + " with session while logged in",
    );

    // Test that we can run a pre-auth command with a session while
    // nobody is logged in (and the session gets ignored)
    db.logout();
    assert.commandWorked(db.runCommand(command), "failed to run command " + cmd + " without being logged in");
    assert.commandWorked(
        db.runCommand(commandWithSession),
        "failed to run command " + cmd + " with session without being logged in",
    );

    db.logout();
    admin.logout();
};

let commands = ["ping", "hello"];
for (let i = 0; i < commands.length; i++) {
    testCommand(commands[i]);
}
MongoRunner.stopMongod(conn, null, {user: "admin", pwd: "pwd"});
