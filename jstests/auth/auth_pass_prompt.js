// Test db.auth with password prompt
(function() {
'use strict';

// Cannot run on windows because of a workaround for runProgram for Windows.
if (!_isWindows()) {
    const conn = MongoRunner.runMongod();
    const test = conn.getDB("test");

    test.createUser({user: "user", pwd: "password", roles: []});
    const user = '"user"';
    const database = '"test"';
    const auth =
        "'var database = db.getMongo().getDB(" + database + "); database.auth(" + user + ");'";

    const binshell = '/bin/sh';
    const mongo = 'mongo';
    const host = conn.host;
    const port = conn.port;
    const ret = runProgram(
        binshell, '-c', `echo password | ${mongo} --host ${host} --port ${port} --eval ${auth}`);

    assert.soon(() => {
        const output = rawMongoProgramOutput();
        return output.includes("Enter password:") && output.includes("Authentication succeeded");
    });

    MongoRunner.stopMongod(conn);
}
}());
