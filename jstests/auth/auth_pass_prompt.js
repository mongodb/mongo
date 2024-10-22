// Test db.auth with password prompt
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
    runProgram(
        binshell, '-c', `echo password | ${mongo} --host ${host} --port ${port} --eval ${auth}`);

    assert.soon(() => {
        const output = rawMongoProgramOutput("(Enter password:|Successfully authenticated)");
        return output.includes("Enter password:") && output.includes("Successfully authenticated");
    });

    MongoRunner.stopMongod(conn);
}
