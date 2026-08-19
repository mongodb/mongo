/**
 * Tests that the 'analyze' command's histogram mode is rejected when test commands are disabled.
 *
 * The histogram mode's behavior when test commands are disabled is covered by this test, which
 * needs a mongod started without test commands and so cannot run in suites that inject failpoint
 * setParameters.
 *
 * TODO SERVER-133281: Remove this tag once the shell stops forwarding failpoint setParameters to
 * servers started without test commands.
 * @tags: [
 *   disables_test_commands,
 * ]
 */

TestData.enableTestCommands = false;
let conn;
try {
    conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");

    const db = conn.getDB(jsTestName());
    const coll = db.test_coll;

    assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));

    let res = db.runCommand({analyze: coll.getName(), key: "a", mode: "histograms"});
    assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);

    res = db.runCommand({analyze: coll.getName(), key: "a"});
    assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);

    coll.drop();
} finally {
    if (conn) {
        MongoRunner.stopMongod(conn);
    }
    TestData.enableTestCommands = true;
}
