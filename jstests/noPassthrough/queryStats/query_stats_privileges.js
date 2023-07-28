/**
 * Test that when test commands are disabled, privileges work as expected.
 * @tags: [featureFlagQueryStats]
 */
load("jstests/aggregation/extras/utils.js");  // For assertAdminDBErrCodeAndErrMsgContains.

(function() {

const runWithConnection = function(enableTestCmds, callback) {
    TestData.enableTestCommands = enableTestCmds;
    assert.eq(jsTest.options().enableTestCommands, enableTestCmds);

    const conn = MongoRunner.runMongod({setParameter: {internalQueryStatsRateLimit: -1}});

    const testDB = conn.getDB('test');
    const coll = testDB[jsTestName()];
    coll.drop();

    callback(testDB, coll);
    MongoRunner.stopMongod(conn);
};

runWithConnection(false /* enableTestCmds **/, (testDB, coll) => {
    const pipeline = [{$queryStats: {}}];
    // We should not be able to run $queryStats without transformation if test commands are
    // disabled.
    assertAdminDBErrCodeAndErrMsgContains(coll,
                                          pipeline,
                                          ErrorCodes.Unauthorized,
                                          "unauthorized to run $queryStats without transformation");

    // Should still be able to run $queryStats with transformation if test commands are disabled.
    assert.commandWorked(testDB.adminCommand({
        aggregate: 1,
        pipeline: [{$queryStats: {transformIdentifiers: {algorithm: "hmac-sha-256"}}}],
        cursor: {}
    }));
});

runWithConnection(true /* enableTestCmds **/, (testDB, coll) => {
    // We should be able to run $queryStats without transformation if test commands are enabled and
    // we are a cluster manager.
    assert.commandWorked(
        testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}));
});
}());
