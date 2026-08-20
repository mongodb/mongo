/**
 * Tests that the 'analyze' command's ndv mode works when test commands are disabled, unlike
 * histograms mode.
 *
 * This needs a mongod started without test commands and so cannot run in suites that inject
 * failpoint setParameters, which are only registered as server parameters when test commands are
 * enabled. See SERVER-132907.
 *
 * TODO SERVER-133281: Remove this tag once the shell stops forwarding failpoint setParameters to
 * servers started without test commands.
 * @tags: [
 *   featureFlagPersistentStats,
 *   disables_test_commands,
 * ]
 */
const kFieldStatsCollName = "system.stats.field_stats";

// 'TestData.enableTestCommands' stays false until the server has been stopped: stopMongod() runs
// the collection validation hook before shutting the server down, and that hook decides whether to
// compare the 'recordIdsReplicated' catalog option based on this flag. The comparison is not
// meaningful against a server started without test commands, so restoring the flag any earlier
// makes the hook fail on variants that replicate record ids.
TestData.enableTestCommands = false;
let conn;
try {
    conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagPersistentStats: true,
            internalQueryEnablePersistentNDVStats: true,
        },
    });
    assert.neq(null, conn, "mongod was unable to start up");

    const db = conn.getDB(jsTestName());
    const coll = db.test_coll;
    assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}]));

    assert.commandWorked(db.runCommand({analyze: coll.getName(), mode: "ndv", key: "a"}));
    const docs = db[kFieldStatsCollName].find().toArray();
    assert.eq(docs.length, 1, "expected a stats doc", {docs});
    assert.eq(docs[0].ndv.sketches[0].ndv, NumberLong(2), "unexpected ndv", {docs});

    // Histograms mode remains a test-only command.
    assert.commandFailedWithCode(
        db.runCommand({analyze: coll.getName(), mode: "histograms", key: "a"}),
        ErrorCodes.CommandNotSupported,
    );

    coll.drop();
} finally {
    if (conn) {
        MongoRunner.stopMongod(conn);
    }
    TestData.enableTestCommands = true;
}
